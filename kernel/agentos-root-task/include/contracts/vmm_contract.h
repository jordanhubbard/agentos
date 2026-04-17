/*
 * VMM (Virtual Machine Monitor) IPC Contract
 *
 * Defines the generic VM lifecycle API shared by all VMM PDs (linux_vmm,
 * freebsd_vmm, and any future VMM PDs).  Opcodes are in agentos.h.
 *
 * Channel: MSG_VM_* opcodes; channel assigned per VMM in agentos.system.
 *
 * Generic VM lifecycle opcodes (MSG_VM_*):
 *   MSG_VM_CREATE   — create a new guest OS slot (returns vm_id)
 *   MSG_VM_DESTROY  — destroy guest OS slot and release all resources
 *   MSG_VM_SWITCH   — set active console output to this guest
 *   MSG_VM_STATUS   — query guest state (CREATING, BOOTING, RUNNING, etc.)
 *   MSG_VM_LIST     — enumerate all guest slots
 *
 * Invariants:
 *   - MSG_VM_CREATE returns a vm_id; all subsequent calls reference it.
 *   - MSG_VM_DESTROY releases ALL device handles held by the guest.
 *   - A guest in DEAD state may not be restarted; destroy and recreate.
 *   - MSG_VM_SWITCH only affects log drain output routing, not IPC routing.
 *   - MSG_VM_LIST results are placed in vmm_shmem region.
 */

#pragma once
#include "../agentos.h"

/* ─── VM state constants ─────────────────────────────────────────────────── */

#define VM_STATE_CREATING  0
#define VM_STATE_BOOTING   1
#define VM_STATE_RUNNING   2
#define VM_STATE_PAUSED    3
#define VM_STATE_DEAD      4

/* ─── OS type constants ──────────────────────────────────────────────────── */

#define VMM_OS_TYPE_FREEBSD  0x02u
#define VMM_OS_TYPE_LINUX    0x01u

/* ─── Request structs ────────────────────────────────────────────────────── */

struct vmm_req_create {
    uint32_t os_type;           /* VMM_OS_TYPE_* */
    uint32_t ram_mb;
    uint32_t device_flags;      /* GUEST_DEV_FLAG_* (see guest_contract.h) */
    uint8_t  os_params[128];    /* OS-specific params (cast to *_create_params_t) */
};

struct vmm_req_destroy {
    uint32_t vm_id;
};

struct vmm_req_switch {
    uint32_t vm_id;
};

struct vmm_req_status {
    uint32_t vm_id;
};

struct vmm_req_list {
    uint32_t max_entries;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct vmm_reply_create {
    uint32_t ok;
    uint32_t vm_id;             /* 0xFFFFFFFF = invalid / error */
};

struct vmm_reply_destroy {
    uint32_t ok;
};

struct vmm_reply_switch {
    uint32_t ok;
};

struct vmm_reply_status {
    uint32_t ok;
    uint32_t state;             /* VM_STATE_* */
    uint32_t ram_used_mb;
    uint32_t uptime_ticks;
};

struct vmm_reply_list {
    uint32_t ok;
    uint32_t count;             /* entries written to vmm_shmem */
};

/* ─── Shmem layout: VM list entry ────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t vm_id;
    uint32_t os_type;
    uint32_t state;
    uint32_t ram_mb;
    uint32_t ram_used_mb;
    uint32_t uptime_ticks;
} vm_list_entry_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum vmm_error {
    VMM_OK                  = 0,
    VMM_ERR_NO_SLOTS        = 1,  /* all VM slots occupied */
    VMM_ERR_BAD_VM_ID       = 2,
    VMM_ERR_BAD_OS_TYPE     = 3,
    VMM_ERR_DEAD            = 4,  /* operation on DEAD guest */
    VMM_ERR_BIND_FAIL       = 5,  /* guest binding protocol failed */
};
