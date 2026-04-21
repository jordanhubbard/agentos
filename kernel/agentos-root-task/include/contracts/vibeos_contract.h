/*
 * VibeOS Lifecycle IPC Contract
 *
 * VibeOS is the top-level OS management API.  It composes device PDs (Phase 2),
 * the guest binding protocol (Phase 3), and the VibeEngine hot-swap mechanism.
 * A caller with the SpawnCap capability can create, manage, and destroy entire
 * OS stacks using only IPC messages.
 *
 * Channel: CH_VIBEENGINE (vibe_engine PD handles VibeOS ops in current build)
 * Opcodes: MSG_VIBEOS_* (see agentos.h)
 *
 * OS creation sequence (MSG_VIBEOS_CREATE):
 *   1. Allocate a swap slot from the swap slot pool.
 *   2. Configure the slot's VMM PD with requested arch/RAM.
 *   3. Wire device PD handles based on device_flags.
 *   4. Execute the guest OS binding protocol (guest_contract.h §3.1).
 *   5. If wasm_hash != 0, hot-load the WASM service via vibe-swap.
 *   6. Publish EVENT_VIBEOS_READY to EventBus.
 *   7. Return vibeos_handle to caller.
 *
 * Invariants:
 *   - vibeos_handle 0 is reserved / invalid.
 *   - MSG_VIBEOS_DESTROY releases ALL resources: VMM PD, device handles, swap slot.
 *   - MSG_VIBEOS_MIGRATE is a live migration; the OS continues running during transfer.
 *   - MSG_VIBEOS_SNAPSHOT and MSG_VIBEOS_RESTORE use AgentFS for checkpoint storage.
 */

#pragma once
#include "../agentos.h"

/* ─── OS type and architecture constants ─────────────────────────────────── */

#define VIBEOS_TYPE_FREEBSD   0x02u
#define VIBEOS_TYPE_LINUX     0x01u

#define VIBEOS_ARCH_AARCH64   0x01u
#define VIBEOS_ARCH_X86_64    0x02u

/* ─── Device flags ───────────────────────────────────────────────────────── */

#define VIBEOS_DEV_SERIAL  (1u << 0)
#define VIBEOS_DEV_NET     (1u << 1)
#define VIBEOS_DEV_BLOCK   (1u << 2)
#define VIBEOS_DEV_USB     (1u << 3)
#define VIBEOS_DEV_FB      (1u << 4)

/* ─── Function class identifiers (mirrors cap_policy.h) ─────────────────── */
/* Used with MSG_VIBEOS_CHECK_SERVICE_EXISTS and non-reinvention enforcement. */
/* dev_type 0..4 (bit positions of VIBEOS_DEV_*) maps to func_class = dev_type+1. */

#define VIBEOS_FUNC_CLASS_SERIAL  0x01u   /* dev_type 0 */
#define VIBEOS_FUNC_CLASS_NET     0x02u   /* dev_type 1 */
#define VIBEOS_FUNC_CLASS_BLOCK   0x03u   /* dev_type 2 */
#define VIBEOS_FUNC_CLASS_USB     0x04u   /* dev_type 3 */
#define VIBEOS_FUNC_CLASS_FB      0x05u   /* dev_type 4 */

/* ─── Module type ────────────────────────────────────────────────────────── */

#define VIBEOS_MODULE_TYPE_WASM  1u
#define VIBEOS_MODULE_TYPE_ELF   2u

/* ─── VibeOS state ───────────────────────────────────────────────────────── */

#define VIBEOS_STATE_CREATING   0
#define VIBEOS_STATE_BOOTING    1
#define VIBEOS_STATE_RUNNING    2
#define VIBEOS_STATE_PAUSED     3
#define VIBEOS_STATE_DEAD       4
#define VIBEOS_STATE_MIGRATING  5

/* ─── Request structs ────────────────────────────────────────────────────── */

struct vibeos_create_req {
    uint8_t  os_type;           /* VIBEOS_TYPE_* */
    uint8_t  arch;              /* VIBEOS_ARCH_* */
    uint8_t  _pad[2];
    uint32_t ram_mb;
    uint32_t cpu_budget_us;     /* MCS scheduling budget per period */
    uint32_t cpu_period_us;
    uint32_t device_flags;      /* VIBEOS_DEV_* bitmask */
    uint8_t  wasm_hash[32];     /* SHA-256 of initial WASM service (0 = none) */
};

struct vibeos_destroy_req {
    uint32_t handle;
};

struct vibeos_status_req {
    uint32_t handle;
};

struct vibeos_list_req {
    uint32_t max_entries;
};

struct vibeos_device_bind_req {
    uint32_t handle;
    uint32_t dev_type;          /* VIBEOS_DEV_* bit position */
    uint32_t dev_handle;        /* handle from device PD open call */
};

struct vibeos_device_unbind_req {
    uint32_t handle;
    uint32_t dev_type;
};

struct vibeos_snapshot_req {
    uint32_t handle;
};

struct vibeos_restore_req {
    uint32_t handle;
    uint32_t snap_lo;           /* snapshot AgentFS inode (low) */
    uint32_t snap_hi;
};

struct vibeos_migrate_req {
    uint32_t handle;
    uint32_t target_node;       /* mesh node_id of destination */
};

struct vibeos_boot_req {
    uint32_t handle;
};

struct vibeos_load_module_req {
    uint32_t handle;
    uint32_t module_type;       /* VIBEOS_MODULE_TYPE_* */
    uint32_t module_size;       /* bytes pre-written to staging region */
    uint8_t  module_hash[32];   /* SHA-256 of module binary */
};

struct vibeos_check_service_req {
    uint32_t func_class;        /* VIBEOS_FUNC_CLASS_* */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct vibeos_create_reply {
    uint32_t ok;
    uint32_t handle;            /* 0 = error */
};

struct vibeos_destroy_reply {
    uint32_t ok;
};

struct vibeos_status_reply {
    uint32_t ok;
    uint32_t state;             /* VIBEOS_STATE_* */
    uint32_t os_type;
    uint32_t ram_mb;
    uint32_t uptime_ticks;
};

struct vibeos_list_reply {
    uint32_t ok;
    uint32_t count;             /* entries written to shmem */
};

struct vibeos_device_bind_reply {
    uint32_t ok;
    uint32_t effective_handle;  /* handle to use — may differ from requested if non-reinvention forced reuse */
    uint32_t preexisting;       /* 1 = existing service found and reused, 0 = new registration */
};

struct vibeos_device_unbind_reply {
    uint32_t ok;
};

struct vibeos_snapshot_reply {
    uint32_t ok;
    uint32_t snap_lo;           /* AgentFS inode of checkpoint (low) */
    uint32_t snap_hi;
};

struct vibeos_restore_reply {
    uint32_t ok;
};

struct vibeos_migrate_reply {
    uint32_t ok;
    uint32_t new_node;          /* node where OS is now running */
};

struct vibeos_boot_reply {
    uint32_t ok;
};

struct vibeos_load_module_reply {
    uint32_t ok;
    uint32_t swap_id;           /* vibe_swap handle on success */
};

struct vibeos_check_service_reply {
    uint32_t ok;
    uint32_t exists;            /* 1 if a ring-0 service is registered for func_class */
    uint32_t pd_handle;         /* existing PD handle (valid when exists=1) */
    uint32_t channel_id;        /* channel for existing PD (valid when exists=1) */
};

/* ─── Shmem layout: VibeOS list entry ───────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t handle;
    uint32_t os_type;
    uint32_t state;
    uint32_t ram_mb;
    uint32_t uptime_ticks;
    uint32_t node_id;           /* mesh node (0 = local) */
} vibeos_info_t;

/* ─── Additional state constants ─────────────────────────────────────────── */

/* VIBEOS_STATE_SNAPSHOT: transient state during snapshot capture */
#define VIBEOS_STATE_SNAPSHOT   6

/* ─── Opcode aliases (VIBEOS_OP_* → MSG_VIBEOS_*) ───────────────────────── */
/*
 * The vos_instance table path (handle_vos_* family) dispatches via
 * VIBEOS_OP_* constants.  They map 1:1 to the MSG_VIBEOS_* wire opcodes
 * defined in agentos.h so callers need only one set of constants.
 */
#define VIBEOS_OP_CREATE         MSG_VIBEOS_CREATE
#define VIBEOS_OP_DESTROY        MSG_VIBEOS_DESTROY
#define VIBEOS_OP_STATUS         MSG_VIBEOS_STATUS
#define VIBEOS_OP_LIST           MSG_VIBEOS_LIST
#define VIBEOS_OP_BIND_DEVICE    MSG_VIBEOS_BIND_DEVICE
#define VIBEOS_OP_UNBIND_DEVICE  MSG_VIBEOS_UNBIND_DEVICE
#define VIBEOS_OP_SNAPSHOT       MSG_VIBEOS_SNAPSHOT
#define VIBEOS_OP_RESTORE        MSG_VIBEOS_RESTORE
#define VIBEOS_OP_MIGRATE        MSG_VIBEOS_MIGRATE
#define VIBEOS_OP_BOOT           MSG_VIBEOS_BOOT
#define VIBEOS_OP_LOAD_MODULE    MSG_VIBEOS_LOAD_MODULE
#define VIBEOS_OP_CHECK_SERVICE  MSG_VIBEOS_CHECK_SERVICE_EXISTS

/* ─── Channel aliases ────────────────────────────────────────────────────── */

/* CH_VIBEOS_ENGINE: authoritative alias for CH_VIBEENGINE (agentos.h) */
#define CH_VIBEOS_ENGINE         CH_VIBEENGINE

/*
 * CH_VMM: the channel from vibe_engine's perspective to the vm_manager PD.
 * vibe_engine.c calls OP_VM_CREATE/DESTROY/START/STOP/INFO/SNAPSHOT/RESTORE
 * via this channel.  Matches CH_VM_MANAGER in agentos.h.
 */
#define CH_VMM                   CH_VM_MANAGER

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum vibeos_error {
    VIBEOS_OK                     = 0,
    VIBEOS_ERR_NO_SLOTS           = 1,
    VIBEOS_ERR_BAD_HANDLE         = 2,
    VIBEOS_ERR_BAD_OS_TYPE        = 3,
    VIBEOS_ERR_DEVICE_UNAVAILABLE = 4,
    VIBEOS_ERR_BIND_FAIL          = 5,
    VIBEOS_ERR_WASM_LOAD_FAIL     = 6,
    VIBEOS_ERR_MIGRATE_FAIL       = 7,
    VIBEOS_ERR_DEAD               = 8,
    VIBEOS_ERR_BAD_MODULE_TYPE    = 9,   /* MSG_VIBEOS_LOAD_MODULE: unknown module_type */
    VIBEOS_ERR_BAD_STATE          = 10,  /* operation not valid for current OS state */
    VIBEOS_ERR_BAD_FUNC_CLASS     = 11,  /* MSG_VIBEOS_CHECK_SERVICE_EXISTS: invalid func_class */

    /* vos_instance path error codes */
    VIBEOS_ERR_BAD_TYPE           = 12,  /* invalid os_type or dev_type bitmask */
    VIBEOS_ERR_OOM                = 13,  /* no free slot or vm_manager refused alloc */
    VIBEOS_ERR_NO_HANDLE          = 14,  /* handle not found in vos_instance table */
    VIBEOS_ERR_WRONG_STATE        = 15,  /* operation invalid for current vos state */
    VIBEOS_ERR_NOT_IMPL           = 16,  /* operation exists in wire format but not yet implemented */
};
