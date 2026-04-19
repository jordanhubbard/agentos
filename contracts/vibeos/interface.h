/*
 * vibeOS API — OS Instance Lifecycle Interface
 *
 * This header defines the complete public API for vibeOS: the primary external
 * interface to agentOS for creating, destroying, configuring, and inspecting
 * guest OS instances on demand.
 *
 * vibeOS sits above the vm_manager / vmm_mux kernel layer and presents a clean,
 * capability-safe IPC surface to consumers. All operations are seL4 Protected
 * Procedure Calls (PPCs) targeting the vibeOS endpoint capability.
 *
 * Endpoint discovery:
 *   The vibeOS endpoint badge is published under the well-known nameserver path
 *   "vibeos.v1". Callers must hold an appropriate capability (minted by the
 *   capability broker) before any operation will succeed.
 *
 * Message encoding:
 *   MR0  — opcode (uint32_t, from VOS_OP_*)
 *   MR1+ — operation-specific input parameters (see per-op comments)
 *   Reply MR0 — VOS_ERR_* status code
 *   Reply MR1+ — operation-specific output
 *
 *   Structured inputs/outputs larger than a few words use a shared-memory
 *   region negotiated at capability attachment time. The caller writes the
 *   input struct to that region before the PPC and reads the output struct
 *   after the reply.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Version ─────────────────────────────────────────────────────────────── */

#define VOS_API_VERSION_MAJOR  0
#define VOS_API_VERSION_MINOR  1
#define VOS_API_VERSION_PATCH  0

/* Encoded version for compile-time comparisons */
#define VOS_API_VERSION \
    ((VOS_API_VERSION_MAJOR << 16) | \
     (VOS_API_VERSION_MINOR <<  8) | \
      VOS_API_VERSION_PATCH)

/* ── Opaque handle ───────────────────────────────────────────────────────── */

/*
 * vos_handle_t — opaque handle to a vibeOS guest OS instance.
 *
 * Returned by VOS_OP_CREATE; required as input for all per-instance
 * operations. A handle of VOS_HANDLE_INVALID indicates no instance.
 *
 * Internally maps to a vm_mux slot index plus generation counter; the
 * value is opaque to consumers and must not be parsed.
 */
typedef uint32_t vos_handle_t;

#define VOS_HANDLE_INVALID  UINT32_C(0xFFFFFFFF)

/* ── OS type enumeration ─────────────────────────────────────────────────── */

/*
 * vos_os_type_t — identifies the guest operating system to boot.
 *
 * VOS_OS_LINUX    — Linux guest via libvmm (AArch64 EL1; x86_64 pending).
 * VOS_OS_FREEBSD  — FreeBSD guest via vmm_mux / freebsd-vmm layer.
 * VOS_OS_CUSTOM   — Caller-supplied boot image; os_type ignored by the VMM,
 *                   boot behaviour determined entirely by boot_image_cap.
 */
typedef enum __attribute__((packed)) {
    VOS_OS_LINUX   = 0,
    VOS_OS_FREEBSD = 1,
    VOS_OS_CUSTOM  = 2,
} vos_os_type_t;

/* ── Instance state enumeration ─────────────────────────────────────────── */

/*
 * vos_state_t — lifecycle state of a guest OS instance.
 *
 *   CREATING   — vm_manager slot allocated; boot in progress.
 *   RUNNING    — guest is scheduled and executing.
 *   SUSPENDED  — guest vCPU(s) are not scheduled (paused or quota-exhausted).
 *   DESTROYED  — slot has been torn down; handle is invalid after this state.
 *
 * Transitions:
 *   CREATING → RUNNING    (boot complete)
 *   RUNNING  → SUSPENDED  (VOS_OP_DESTROY-with-suspend, quota exhausted)
 *   SUSPENDED→ RUNNING    (VOS_OP_RESTORE or resume trigger)
 *   RUNNING  → DESTROYED  (VOS_OP_DESTROY)
 *   SUSPENDED→ DESTROYED  (VOS_OP_DESTROY)
 */
typedef enum __attribute__((packed)) {
    VOS_STATE_CREATING  = 0,
    VOS_STATE_RUNNING   = 1,
    VOS_STATE_SUSPENDED = 2,
    VOS_STATE_DESTROYED = 3,
} vos_state_t;

/* ── Service type bitmask ────────────────────────────────────────────────── */

/*
 * vos_service_type_t — identifies a virtual device service that can be
 * attached to or detached from a guest OS instance.
 *
 * These map directly to virtio device types exposed by the VMM layer.
 * Multiple services may be attached concurrently; the vos_status_t
 * bound_services field is a bitmask of VOS_SVC_* values.
 */
typedef enum __attribute__((packed)) {
    VOS_SVC_SERIAL  = (1u << 0),  /* virtio-console / UART                  */
    VOS_SVC_NETWORK = (1u << 1),  /* virtio-net (bridged or isolated)        */
    VOS_SVC_BLOCK   = (1u << 2),  /* virtio-blk (agentfs-backed)             */
    VOS_SVC_USB     = (1u << 3),  /* virtio-usb pass-through                 */
    VOS_SVC_TIMER   = (1u << 4),  /* high-resolution timer injection         */
    VOS_SVC_ENTROPY = (1u << 5),  /* virtio-rng seeded from agentOS entropy  */
} vos_service_type_t;

/* ── Opcode constants ────────────────────────────────────────────────────── */

/*
 * vibeOS opcodes are placed in the 0x5600 range to avoid collision with
 * existing agentOS IPC labels (see agentos.h MSG_* and OP_VM_* constants).
 *
 * All opcodes are sent in MR0 of the seL4 PPC to the vibeOS endpoint.
 */
#define VOS_OP_BASE             UINT32_C(0x5600)

#define VOS_OP_CREATE           (VOS_OP_BASE + 0x00u)  /* Create a new OS instance  */
#define VOS_OP_DESTROY          (VOS_OP_BASE + 0x01u)  /* Destroy an OS instance    */
#define VOS_OP_STATUS           (VOS_OP_BASE + 0x02u)  /* Query instance status     */
#define VOS_OP_LIST             (VOS_OP_BASE + 0x03u)  /* List all live handles     */
#define VOS_OP_ATTACH_SERVICE   (VOS_OP_BASE + 0x04u)  /* Attach a virtual service  */
#define VOS_OP_DETACH_SERVICE   (VOS_OP_BASE + 0x05u)  /* Detach a virtual service  */
#define VOS_OP_SNAPSHOT         (VOS_OP_BASE + 0x06u)  /* Snapshot instance memory  */
#define VOS_OP_RESTORE          (VOS_OP_BASE + 0x07u)  /* Restore from snapshot     */
#define VOS_OP_MIGRATE          (VOS_OP_BASE + 0x08u)  /* Migrate to another domain */
#define VOS_OP_CONFIGURE        (VOS_OP_BASE + 0x09u)  /* Reconfigure live instance */

/* ── Error codes ─────────────────────────────────────────────────────────── */

/*
 * All vibeOS operations return a VOS_ERR_* code in reply MR0.
 * VOS_ERR_OK (0) indicates success; all other values indicate failure.
 * On failure, operation-specific output registers are undefined.
 */
typedef uint32_t vos_err_t;

#define VOS_ERR_OK                UINT32_C(0)   /* Success                              */
#define VOS_ERR_INVALID_HANDLE    UINT32_C(1)   /* Handle does not identify a live inst */
#define VOS_ERR_OUT_OF_MEMORY     UINT32_C(2)   /* Insufficient untyped memory          */
#define VOS_ERR_PERMISSION_DENIED UINT32_C(3)   /* Caller badge lacks required rights   */
#define VOS_ERR_INVALID_SPEC      UINT32_C(4)   /* vos_spec_t field(s) out of range     */
#define VOS_ERR_UNSUPPORTED_OS    UINT32_C(5)   /* vos_os_type_t not supported on arch  */
#define VOS_ERR_SERVICE_UNAVAIL   UINT32_C(6)   /* Requested service not available      */
#define VOS_ERR_ALREADY_EXISTS    UINT32_C(7)   /* Duplicate create with same label     */
#define VOS_ERR_SNAPSHOT_FAILED   UINT32_C(8)   /* Snapshot serialisation error         */
#define VOS_ERR_MIGRATE_FAILED    UINT32_C(9)   /* Migration to dest domain failed      */
#define VOS_ERR_NOT_SUPPORTED     UINT32_C(10)  /* Op valid but not impl on this build  */
#define VOS_ERR_INTERNAL          UINT32_C(99)  /* Unexpected internal error (bug)      */

/* ── Core structures ─────────────────────────────────────────────────────── */

/*
 * vos_spec_t — specification for a new OS instance.
 *
 * Passed by the caller to VOS_OP_CREATE via the negotiated shared-memory
 * region (offset 0). The vibeOS server reads this struct atomically; the
 * caller must ensure all fields are initialised before the PPC.
 *
 * Fields:
 *   os_type         — which guest OS to boot (see vos_os_type_t)
 *   memory_pages    — RAM to allocate for the guest, in 4 KiB pages.
 *                     Must be in [VOS_SPEC_MIN_PAGES, VOS_SPEC_MAX_PAGES].
 *   vcpu_count      — number of virtual CPUs. Currently only 1 is supported;
 *                     >1 is accepted for future SMP guests.
 *   boot_image_cap  — seL4 capability index pointing to the guest boot image
 *                     (kernel + optional initrd) in the caller's CSpace.
 *                     For VOS_OS_LINUX / VOS_OS_FREEBSD, the VMM layer
 *                     expects a flat binary or ELF at offset 0.
 *   config_blob     — pointer (in caller address space) to an optional opaque
 *                     configuration blob. Interpreted per os_type:
 *                     Linux  — kernel command-line string (NUL-terminated)
 *                     FreeBSD— loader.conf fragment
 *                     Custom — passed verbatim to the boot image
 *                     NULL is valid; config_len must be 0 in that case.
 *   config_len      — byte length of config_blob. 0 means no config.
 *   label           — human-readable NUL-terminated label (max 15 chars + NUL).
 *                     Used in debug output and OP_VM_LIST; need not be unique
 *                     but duplicate labels produce VOS_ERR_ALREADY_EXISTS when
 *                     the server is configured to enforce uniqueness.
 *   cpu_quota_pct   — CPU share for this instance, 0-100. Maps directly to
 *                     vm_slot_quota_t.max_cpu_pct in the scheduler. 0 means
 *                     the guest gets CPU only when no other guest is runnable.
 *   cpu_affinity    — bitmask of host CPUs the guest vCPU may run on.
 *                     0xFFFFFFFF means "any core" (default).
 */
typedef struct __attribute__((packed)) {
    vos_os_type_t   os_type;           /* 1 byte  */
    uint8_t         vcpu_count;        /* 1 byte  */
    uint8_t         cpu_quota_pct;     /* 1 byte: 0-100 */
    uint8_t         _pad0;             /* alignment pad */
    uint32_t        memory_pages;      /* 4 bytes: number of 4 KiB pages */
    uint32_t        cpu_affinity;      /* 4 bytes: host CPU bitmask */
    uint32_t        boot_image_cap;    /* 4 bytes: seL4 cap index */
    uint32_t        config_len;        /* 4 bytes: length of config_blob */
    uint64_t        config_blob;       /* 8 bytes: ptr (caller address space) */
    char            label[16];         /* 16 bytes: NUL-terminated label */
} vos_spec_t;                          /* total: 44 bytes */

/* Constraints on vos_spec_t.memory_pages */
#define VOS_SPEC_MIN_PAGES   UINT32_C(256)     /* 1 MiB minimum guest RAM  */
#define VOS_SPEC_MAX_PAGES   UINT32_C(524288)  /* 2 GiB maximum guest RAM  */

/* Maximum number of simultaneous guest instances (matches VM_MAX_SLOTS) */
#define VOS_MAX_INSTANCES    4u

/*
 * vos_status_t — status reply for VOS_OP_STATUS.
 *
 * Written by vibeOS to the shared-memory region at offset 0 before the PPC
 * reply. The caller reads this struct after the reply with MR0 == VOS_ERR_OK.
 *
 * Fields:
 *   handle           — echoes back the queried handle
 *   state            — current lifecycle state (see vos_state_t)
 *   os_type          — OS type of this instance
 *   _pad             — reserved, zero
 *   uptime_ms        — milliseconds since the instance entered VOS_STATE_RUNNING.
 *                      Zero if still in CREATING or already DESTROYED.
 *   memory_used_pages— pages consumed by the guest (RAM + device mappings).
 *                      Approximated from vm_stats_t.
 *   memory_total_pages— total pages allocated to the guest (from vos_spec_t).
 *   run_ticks        — scheduler ticks this instance was active (from vm_stats_t).
 *   preempt_count    — times this instance was preempted by the scheduler.
 *   bound_services   — bitmask of currently attached services (VOS_SVC_* bits).
 *   cpu_quota_pct    — current CPU quota (may differ from spec if reconfigured).
 *   vcpu_count       — number of vCPUs in this instance.
 *   label            — NUL-terminated label from vos_spec_t at creation time.
 */
typedef struct __attribute__((packed)) {
    vos_handle_t    handle;              /* 4 bytes */
    vos_state_t     state;              /* 1 byte  */
    vos_os_type_t   os_type;            /* 1 byte  */
    uint8_t         cpu_quota_pct;      /* 1 byte  */
    uint8_t         vcpu_count;         /* 1 byte  */
    uint64_t        uptime_ms;          /* 8 bytes */
    uint32_t        memory_used_pages;  /* 4 bytes */
    uint32_t        memory_total_pages; /* 4 bytes */
    uint64_t        run_ticks;          /* 8 bytes */
    uint64_t        preempt_count;      /* 8 bytes */
    uint32_t        bound_services;     /* 4 bytes: VOS_SVC_* bitmask */
    uint32_t        _pad;               /* 4 bytes: reserved */
    char            label[16];          /* 16 bytes */
} vos_status_t;                         /* total: 64 bytes */

/*
 * vos_list_entry_t — one element in the array returned by VOS_OP_LIST.
 *
 * VOS_OP_LIST writes an array of vos_list_entry_t into the shared-memory
 * region at offset 0, preceded by a uint32_t count at offset 0. Callers
 * should read the count first, then iterate the entries immediately following.
 *
 * Layout in shared memory:
 *   [0x00] uint32_t count
 *   [0x04] vos_list_entry_t entries[count]
 */
typedef struct __attribute__((packed)) {
    vos_handle_t    handle;    /* 4 bytes */
    vos_state_t     state;     /* 1 byte  */
    vos_os_type_t   os_type;   /* 1 byte  */
    uint8_t         _pad[2];   /* 2 bytes */
    char            label[16]; /* 16 bytes */
} vos_list_entry_t;            /* total: 24 bytes */

/* ── Per-operation IPC layouts ───────────────────────────────────────────── */

/*
 * VOS_OP_CREATE
 *   Create a new guest OS instance from a specification.
 *
 *   Input (shared memory [0]):
 *     vos_spec_t spec
 *
 *   Input MRs:
 *     MR0 = VOS_OP_CREATE
 *     (no additional MRs; all parameters are in the spec struct)
 *
 *   Output MRs on success (MR0 == VOS_ERR_OK):
 *     MR1 = new vos_handle_t
 *
 *   Errors:
 *     VOS_ERR_OUT_OF_MEMORY     — no free vm_mux slot or insufficient RAM
 *     VOS_ERR_INVALID_SPEC      — memory_pages out of range, bad vcpu_count
 *     VOS_ERR_UNSUPPORTED_OS    — os_type not supported on this arch/build
 *     VOS_ERR_PERMISSION_DENIED — caller capability lacks create right
 *     VOS_ERR_ALREADY_EXISTS    — label collision (if uniqueness enforced)
 */

/*
 * VOS_OP_DESTROY
 *   Destroy a guest OS instance and reclaim all associated resources.
 *   The handle is invalidated upon success.
 *
 *   Input MRs:
 *     MR0 = VOS_OP_DESTROY
 *     MR1 = vos_handle_t handle
 *
 *   Output MRs on success:
 *     MR0 = VOS_ERR_OK
 *
 *   Errors:
 *     VOS_ERR_INVALID_HANDLE    — handle does not identify a live instance
 *     VOS_ERR_PERMISSION_DENIED — caller capability lacks destroy right
 */

/*
 * VOS_OP_STATUS
 *   Query the current status of a guest OS instance.
 *
 *   Input MRs:
 *     MR0 = VOS_OP_STATUS
 *     MR1 = vos_handle_t handle
 *
 *   Output (shared memory [0]) on success:
 *     vos_status_t status
 *
 *   Output MRs on success:
 *     MR0 = VOS_ERR_OK
 *
 *   Errors:
 *     VOS_ERR_INVALID_HANDLE    — handle does not identify a live instance
 */

/*
 * VOS_OP_LIST
 *   List all currently live OS instance handles.
 *
 *   Input MRs:
 *     MR0 = VOS_OP_LIST
 *
 *   Output (shared memory [0]) on success:
 *     uint32_t count
 *     vos_list_entry_t entries[count]   (immediately following the count)
 *
 *   Output MRs on success:
 *     MR0 = VOS_ERR_OK
 *     MR1 = count (also present in shared memory for convenience)
 *
 *   Notes:
 *     The count may be 0 if no instances are running.
 *     count is bounded by VOS_MAX_INSTANCES.
 */

/*
 * VOS_OP_ATTACH_SERVICE
 *   Attach a virtual device service to a running OS instance.
 *
 *   Input MRs:
 *     MR0 = VOS_OP_ATTACH_SERVICE
 *     MR1 = vos_handle_t handle
 *     MR2 = vos_service_type_t service_type
 *
 *   Output MRs on success:
 *     MR0 = VOS_ERR_OK
 *     MR1 = capability token for the attached service (seL4 cap index)
 *           This cap allows the caller to interact directly with the service
 *           (e.g., write to a serial console or read from a block device).
 *
 *   Errors:
 *     VOS_ERR_INVALID_HANDLE    — handle invalid
 *     VOS_ERR_SERVICE_UNAVAIL   — service type not available on this build
 *     VOS_ERR_ALREADY_EXISTS    — service already attached to this instance
 *     VOS_ERR_PERMISSION_DENIED — caller lacks service attach right
 */

/*
 * VOS_OP_DETACH_SERVICE
 *   Detach a virtual device service from a running OS instance.
 *
 *   Input MRs:
 *     MR0 = VOS_OP_DETACH_SERVICE
 *     MR1 = vos_handle_t handle
 *     MR2 = vos_service_type_t service_type
 *
 *   Output MRs on success:
 *     MR0 = VOS_ERR_OK
 *
 *   Errors:
 *     VOS_ERR_INVALID_HANDLE    — handle invalid
 *     VOS_ERR_SERVICE_UNAVAIL   — service was not attached
 */

/*
 * VOS_OP_SNAPSHOT
 *   Capture a point-in-time snapshot of a guest OS instance's memory state.
 *   The guest is briefly suspended during the capture window.
 *
 *   Input MRs:
 *     MR0 = VOS_OP_SNAPSHOT
 *     MR1 = vos_handle_t handle
 *     MR2 = dest_store_cap — seL4 cap index pointing to the destination
 *           storage object (agentfs-backed or caller-supplied frame).
 *
 *   Output MRs on success:
 *     MR0 = VOS_ERR_OK
 *     MR1 = snapshot_id_lo (low 32 bits of 64-bit snapshot ID / content hash)
 *     MR2 = snapshot_id_hi (high 32 bits)
 *
 *   Errors:
 *     VOS_ERR_INVALID_HANDLE    — handle invalid
 *     VOS_ERR_SNAPSHOT_FAILED   — serialisation error, no space, or I/O error
 *     VOS_ERR_PERMISSION_DENIED — caller lacks snapshot right
 */

/*
 * VOS_OP_RESTORE
 *   Create a new OS instance initialised from a previously captured snapshot.
 *
 *   Input (shared memory [0]):
 *     vos_spec_t spec   — resource specification for the restored instance.
 *                         os_type and memory_pages must match the snapshot.
 *                         boot_image_cap is ignored (image comes from snapshot).
 *
 *   Input MRs:
 *     MR0 = VOS_OP_RESTORE
 *     MR1 = snapshot_id_lo
 *     MR2 = snapshot_id_hi
 *
 *   Output MRs on success:
 *     MR0 = VOS_ERR_OK
 *     MR1 = new vos_handle_t for the restored instance
 *
 *   Errors:
 *     VOS_ERR_INVALID_SPEC      — spec resource fields incompatible with snapshot
 *     VOS_ERR_OUT_OF_MEMORY     — no free slot or insufficient RAM
 *     VOS_ERR_SNAPSHOT_FAILED   — snapshot not found or corrupted
 */

/*
 * VOS_OP_MIGRATE
 *   Transfer a live guest OS instance to a different protection domain or
 *   physical node (domain capability). The handle in the source domain is
 *   invalidated on success; a new handle is returned for the destination.
 *
 *   Input MRs:
 *     MR0 = VOS_OP_MIGRATE
 *     MR1 = vos_handle_t source_handle
 *     MR2 = dest_domain_cap — capability identifying the destination vibeOS
 *           endpoint (may be on another physical host via the mesh layer).
 *
 *   Output MRs on success:
 *     MR0 = VOS_ERR_OK
 *     MR1 = new vos_handle_t valid in the destination domain
 *
 *   Errors:
 *     VOS_ERR_INVALID_HANDLE    — source handle invalid
 *     VOS_ERR_MIGRATE_FAILED    — dest unreachable, out of resources, or
 *                                  state transfer error
 *     VOS_ERR_NOT_SUPPORTED     — migration not supported on this build
 *     VOS_ERR_PERMISSION_DENIED — caller lacks migrate right
 */

/*
 * VOS_OP_CONFIGURE
 *   Reconfigure a live guest OS instance without destroying it. The semantics
 *   are os_type-specific: for Linux this updates the kernel parameters visible
 *   via /proc/cmdline on next reboot; for block/network devices it adjusts
 *   the virtio device parameters.
 *
 *   Input (shared memory [0]):
 *     uint32_t config_len         — byte length of the config blob
 *     uint8_t  config_blob[...]   — opaque blob, interpreted per os_type
 *
 *   Input MRs:
 *     MR0 = VOS_OP_CONFIGURE
 *     MR1 = vos_handle_t handle
 *     MR2 = config_len (also present in shared memory; MR value is canonical)
 *
 *   Output MRs on success:
 *     MR0 = VOS_ERR_OK
 *
 *   Errors:
 *     VOS_ERR_INVALID_HANDLE    — handle invalid
 *     VOS_ERR_INVALID_SPEC      — config blob rejected by the VMM layer
 *     VOS_ERR_PERMISSION_DENIED — caller lacks configure right
 */

/* ── Capability rights bitmask ───────────────────────────────────────────── */

/*
 * vos_rights_t — bitmask of rights encoded in a vibeOS capability badge.
 *
 * The capability broker mints endpoint capabilities badged with these rights.
 * vibeOS checks the badge on every PPC. Callers without the required right
 * for an operation receive VOS_ERR_PERMISSION_DENIED.
 *
 * VOS_RIGHT_CREATE   — may call VOS_OP_CREATE
 * VOS_RIGHT_DESTROY  — may call VOS_OP_DESTROY
 * VOS_RIGHT_INSPECT  — may call VOS_OP_STATUS, VOS_OP_LIST
 * VOS_RIGHT_ATTACH   — may call VOS_OP_ATTACH_SERVICE, VOS_OP_DETACH_SERVICE
 * VOS_RIGHT_SNAPSHOT — may call VOS_OP_SNAPSHOT, VOS_OP_RESTORE
 * VOS_RIGHT_MIGRATE  — may call VOS_OP_MIGRATE
 * VOS_RIGHT_CONFIGURE— may call VOS_OP_CONFIGURE
 * VOS_RIGHT_ALL      — all rights (convenience for trusted callers)
 */
typedef uint32_t vos_rights_t;

#define VOS_RIGHT_CREATE    (1u << 0)
#define VOS_RIGHT_DESTROY   (1u << 1)
#define VOS_RIGHT_INSPECT   (1u << 2)
#define VOS_RIGHT_ATTACH    (1u << 3)
#define VOS_RIGHT_SNAPSHOT  (1u << 4)
#define VOS_RIGHT_MIGRATE   (1u << 5)
#define VOS_RIGHT_CONFIGURE (1u << 6)

#define VOS_RIGHT_ALL       (VOS_RIGHT_CREATE    | \
                             VOS_RIGHT_DESTROY   | \
                             VOS_RIGHT_INSPECT   | \
                             VOS_RIGHT_ATTACH    | \
                             VOS_RIGHT_SNAPSHOT  | \
                             VOS_RIGHT_MIGRATE   | \
                             VOS_RIGHT_CONFIGURE)

/* ── Static assertions (C11) ─────────────────────────────────────────────── */
#ifdef __STDC_VERSION__
#if __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(vos_spec_t)       == 44, "vos_spec_t size mismatch");
_Static_assert(sizeof(vos_status_t)     == 64, "vos_status_t size mismatch");
_Static_assert(sizeof(vos_list_entry_t) == 24, "vos_list_entry_t size mismatch");
_Static_assert(sizeof(vos_os_type_t)    ==  1, "vos_os_type_t must be 1 byte");
_Static_assert(sizeof(vos_state_t)      ==  1, "vos_state_t must be 1 byte");
_Static_assert(sizeof(vos_service_type_t) == 1, "vos_service_type_t must be 1 byte");
#endif
#endif
