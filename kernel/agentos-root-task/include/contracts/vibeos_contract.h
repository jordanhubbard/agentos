/* SPDX-License-Identifier: GPL-2.0-only */
/* contracts/vibeos_contract.h — Phase 4: VibeOS top-level OS lifecycle API
 *
 * VibeOS is the primary external interface to agentOS.  It composes the
 * guest binding protocol (guest_contract.h) and the generic VMM lifecycle
 * (vmm_contract.h) into a single, stable API callable by any protection
 * domain that holds the vibeOS endpoint capability.
 *
 * Wire format: the caller places the opcode in MR0, fills the typed request
 * struct into MR1..MRn via seL4_SetMR(), calls seL4_Call(), and reads the
 * typed reply struct from MR0..MRn via seL4_GetMR().  All structs are
 * __attribute__((packed)); no implicit padding.
 *
 * Version history:
 *   1  —  initial definition (Phase 4)
 *          VOS_MIGRATE present in opcode table but returns VIBEOS_ERR_NOT_IMPL
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "guest_contract.h"

#define VIBEOS_CONTRACT_VERSION 1

/* -------------------------------------------------------------------------
 * VibeOS lifecycle opcodes — MR0 value in every vibeOS endpoint call
 * ---------------------------------------------------------------------- */
#define VIBEOS_OP_CREATE        0xB001u  /* instantiate a new guest OS */
#define VIBEOS_OP_DESTROY       0xB002u  /* tear down and reclaim all caps */
#define VIBEOS_OP_STATUS        0xB003u  /* query state and resource usage */
#define VIBEOS_OP_LIST          0xB004u  /* enumerate active vibeOS handles */
#define VIBEOS_OP_BIND_DEVICE   0xB005u  /* attach an additional device PD */
#define VIBEOS_OP_UNBIND_DEVICE 0xB006u  /* detach a device PD at runtime */
#define VIBEOS_OP_SNAPSHOT      0xB007u  /* checkpoint OS state to storage */
#define VIBEOS_OP_RESTORE       0xB008u  /* restore from a prior snapshot */
#define VIBEOS_OP_MIGRATE       0xB009u  /* move instance between cap domains (Phase 4+) */

/* -------------------------------------------------------------------------
 * VibeOS instance state — reported in vos_status_reply_t.state
 * ---------------------------------------------------------------------- */
typedef enum {
    VIBEOS_STATE_CREATING  = 0,  /* vos_create_req_t accepted; VMM PD being set up */
    VIBEOS_STATE_BOOTING   = 1,  /* VMM PD running; guest OS not yet ready */
    VIBEOS_STATE_RUNNING   = 2,  /* EVENT_GUEST_READY received; guest OS is live */
    VIBEOS_STATE_PAUSED    = 3,  /* vCPU scheduling suspended */
    VIBEOS_STATE_DEAD      = 4,  /* terminated; handle is invalid */
    VIBEOS_STATE_MIGRATING = 5,  /* live migration in progress (Phase 4+) */
    VIBEOS_STATE_SNAPSHOT  = 6,  /* snapshot I/O in progress; other ops may block */
} vibeos_state_t;

/* -------------------------------------------------------------------------
 * VIBEOS_OP_CREATE
 *
 * Allocates a vibeOS handle, runs the full guest binding protocol, and
 * (if wasm_hash is non-zero) triggers a vibe-engine hot-load after the
 * guest OS reaches VIBEOS_STATE_RUNNING.
 * ---------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t opcode;           /* VIBEOS_OP_CREATE */
    uint8_t  os_type;          /* guest_type_t cast to uint8_t */
    uint8_t  arch;             /* 0 = aarch64, 1 = x86_64 */
    uint8_t  _pad[2];
    uint32_t ram_mb;           /* physical RAM in MiB */
    uint32_t cpu_budget_us;    /* seL4 MCS scheduling budget in microseconds */
    uint32_t cpu_period_us;    /* seL4 MCS scheduling period in microseconds */
    uint32_t device_flags;     /* GUEST_DEV_* bitmask of devices to bind at create */
    uint8_t  wasm_hash[32];    /* SHA-256 of WASM component to hot-load post-boot;
                                * all-zeros means no initial hot-load */
    uint8_t  label[32];        /* human-readable name, null-terminated */
} vos_create_req_t;

typedef struct __attribute__((packed)) {
    uint32_t result;  /* 0 on success; vibeos_error_t on failure */
    uint32_t handle;  /* opaque 32-bit vibeOS instance ID, valid until DESTROY */
} vos_create_reply_t;

/* -------------------------------------------------------------------------
 * VIBEOS_OP_DESTROY
 * ---------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t opcode;  /* VIBEOS_OP_DESTROY */
    uint32_t handle;
} vos_destroy_req_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
} vos_destroy_reply_t;

/* -------------------------------------------------------------------------
 * VIBEOS_OP_STATUS
 * ---------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t opcode;  /* VIBEOS_OP_STATUS */
    uint32_t handle;
} vos_status_req_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t handle;
    uint8_t  state;     /* vibeos_state_t cast to uint8_t */
    uint8_t  os_type;   /* guest_type_t cast to uint8_t */
    uint8_t  _pad[2];
    uint32_t ram_mb;
    uint32_t dev_mask;  /* GUEST_DEV_* bitmask of currently bound devices */
    uint64_t uptime_ms; /* milliseconds since VIBEOS_STATE_RUNNING first entered */
} vos_status_reply_t;

/* -------------------------------------------------------------------------
 * VIBEOS_OP_LIST — returns up to 16 active handles per call.
 * Re-issue with offset incremented to page through more than 16 instances.
 * ---------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t opcode;  /* VIBEOS_OP_LIST */
    uint32_t offset;  /* index of first handle to return; 0 for initial call */
} vos_list_req_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t count;        /* number of valid entries in handles[] */
    uint32_t handles[16];
} vos_list_reply_t;

/* -------------------------------------------------------------------------
 * VIBEOS_OP_BIND_DEVICE / VIBEOS_OP_UNBIND_DEVICE
 *
 * Attaches or detaches a single generic device PD to/from a running
 * vibeOS instance.  The dev_type field must contain exactly one GUEST_DEV_*
 * bit; passing multiple bits or zero is an error.
 * ---------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t opcode;    /* VIBEOS_OP_BIND_DEVICE or VIBEOS_OP_UNBIND_DEVICE */
    uint32_t handle;
    uint32_t dev_type;  /* exactly one GUEST_DEV_* bit */
} vos_device_req_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
} vos_device_reply_t;

/* -------------------------------------------------------------------------
 * VIBEOS_OP_SNAPSHOT
 * ---------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t opcode;  /* VIBEOS_OP_SNAPSHOT */
    uint32_t handle;
} vos_snapshot_req_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t handle;
    uint8_t  snap_hash[32]; /* SHA-256 identifier for the written snapshot blob */
} vos_snapshot_reply_t;

/* -------------------------------------------------------------------------
 * VIBEOS_OP_RESTORE
 * ---------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t opcode;        /* VIBEOS_OP_RESTORE */
    uint32_t handle;
    uint8_t  snap_hash[32];
} vos_restore_req_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
} vos_restore_reply_t;

/* -------------------------------------------------------------------------
 * VIBEOS_OP_MIGRATE — Phase 4+ placeholder
 *
 * Currently returns VIBEOS_ERR_NOT_IMPL.  Request and reply structs are
 * defined here so callers can be written to the final interface now.
 * ---------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VIBEOS_OP_MIGRATE */
    uint32_t handle;
    uint32_t target_domain;   /* destination capability domain ID */
} vos_migrate_req_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t new_handle;      /* handle in the target domain after migration */
} vos_migrate_reply_t;

/* -------------------------------------------------------------------------
 * EventBus events published by the vibeOS service
 * ---------------------------------------------------------------------- */
#define EVENT_VIBEOS_READY 0x5001u  /* instance reached VIBEOS_STATE_RUNNING */
#define EVENT_VIBEOS_DEAD  0x5002u  /* instance terminated; handle is invalid */

/* -------------------------------------------------------------------------
 * Error codes returned in result fields
 * ---------------------------------------------------------------------- */
typedef enum {
    VIBEOS_OK              = 0,
    VIBEOS_ERR_NO_HANDLE   = 1,  /* handle does not refer to a live instance */
    VIBEOS_ERR_BAD_TYPE    = 2,  /* os_type or arch value is unrecognised */
    VIBEOS_ERR_OOM         = 3,  /* insufficient memory or VMM slots */
    VIBEOS_ERR_DEV_UNAVAIL = 4,  /* requested device PD is not ready */
    VIBEOS_ERR_NOT_IMPL    = 5,  /* operation defined but not yet implemented */
    VIBEOS_ERR_WRONG_STATE = 6,  /* operation not valid in instance's current state */
} vibeos_error_t;

/* -------------------------------------------------------------------------
 * Invariants (enforced by the vibeOS service implementation):
 *
 *  I1. handle values are assigned by the vibeOS service and are never reused
 *      within a boot session.  A handle returned by VIBEOS_OP_CREATE remains
 *      valid until VIBEOS_OP_DESTROY returns VIBEOS_OK for that handle.
 *
 *  I2. A caller may not issue any opcode other than VIBEOS_OP_STATUS or
 *      VIBEOS_OP_DESTROY against an instance in VIBEOS_STATE_CREATING or
 *      VIBEOS_STATE_BOOTING.
 *
 *  I3. VIBEOS_OP_MIGRATE returns VIBEOS_ERR_NOT_IMPL until Phase 4 migration
 *      support is implemented and the contract version is bumped to 2.
 *
 *  I4. If wasm_hash in vos_create_req_t is non-zero, the vibeOS service
 *      forwards it to the vibe-engine (contracts/vibe-engine/) after the
 *      instance reaches VIBEOS_STATE_RUNNING.  Failure of the hot-load does
 *      not destroy the instance; it is reported via the EventBus.
 *
 *  I5. VIBEOS_OP_SNAPSHOT transitions the instance to VIBEOS_STATE_SNAPSHOT
 *      for the duration of the I/O.  Other mutating operations issued during
 *      this window return VIBEOS_ERR_WRONG_STATE.
 *
 *  I6. vos_device_req_t.dev_type must contain exactly one GUEST_DEV_* bit.
 *      Zero or multiple bits set returns VIBEOS_ERR_BAD_TYPE.
 * ---------------------------------------------------------------------- */
