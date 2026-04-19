/* SPDX-License-Identifier: GPL-2.0-only */
/* contracts/vmm_contract.h — Phase 3: Generic VM lifecycle contract
 *
 * Defines the shared IPC opcode table and message structs used by ALL
 * VMM protection domains (linux_vmm.c, freebsd_vmm.c, and future VMMs).
 *
 * Each VMM PD exposes a single seL4 endpoint.  The caller places the
 * opcode in MR0 (message register 0) and the typed request struct in
 * MR1..MRn via seL4_SetMR().  The VMM PD replies with the typed reply
 * struct beginning at MR0.
 *
 * Opcodes are numerically identical to the OP_VM_* constants previously
 * scattered across vm_manager.c and individual VMM sources; this header
 * is the single authoritative definition.
 *
 * Version history:
 *   1  —  initial definition (Phase 3), consolidating per-VMM opcodes
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "guest_contract.h"

#define VMM_CONTRACT_VERSION 1

/* -------------------------------------------------------------------------
 * Generic VM lifecycle opcodes — MR0 value in every VMM PD endpoint call
 * ---------------------------------------------------------------------- */
#define VMM_OP_CREATE   0x10u  /* allocate a new VM slot and begin setup */
#define VMM_OP_DESTROY  0x11u  /* tear down a VM and reclaim all resources */
#define VMM_OP_START    0x12u  /* begin execution of a created-but-idle VM */
#define VMM_OP_STOP     0x13u  /* halt a running VM (state preserved) */
#define VMM_OP_PAUSE    0x14u  /* suspend vCPU scheduling; memory intact */
#define VMM_OP_RESUME   0x15u  /* resume a paused VM */
/* 0x16 reserved for future VMM_OP_RESET */
#define VMM_OP_INFO     0x17u  /* query state and attributes of one VM slot */
#define VMM_OP_LIST     0x18u  /* enumerate active VM slots (up to 8 per call) */
#define VMM_OP_SNAPSHOT 0x19u  /* checkpoint VM state to block storage */
#define VMM_OP_RESTORE  0x1Au  /* restore VM from a prior snapshot */

/* -------------------------------------------------------------------------
 * VM state — reported in vmm_reply_info_t.state
 * ---------------------------------------------------------------------- */
typedef enum {
    VM_STATE_EMPTY    = 0,  /* slot is free */
    VM_STATE_CREATING = 1,  /* slot allocated; guest binding in progress */
    VM_STATE_RUNNING  = 2,  /* vCPU is executing guest code */
    VM_STATE_PAUSED   = 3,  /* vCPU scheduling suspended; memory intact */
    VM_STATE_DEAD     = 4,  /* terminated; slot will be reclaimed */
} vm_state_t;

/* -------------------------------------------------------------------------
 * VMM_OP_CREATE
 * ---------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t opcode;        /* VMM_OP_CREATE */
    uint8_t  guest_type;    /* guest_type_t cast to uint8_t */
    uint8_t  _pad[3];
    uint32_t ram_mb;        /* physical RAM to allocate in MiB */
    uint32_t cpu_budget_us; /* seL4 MCS scheduling budget in microseconds */
    uint32_t cpu_period_us; /* seL4 MCS scheduling period in microseconds */
    uint32_t dev_mask;      /* GUEST_DEV_* bitmask: which device PDs to bind */
    uint8_t  label[32];     /* human-readable instance name, null-terminated */
} vmm_req_create_t;

typedef struct __attribute__((packed)) {
    uint32_t result;   /* 0 on success; vmm_error_t on failure */
    uint32_t slot_id;  /* opaque slot identifier, valid until DESTROY succeeds */
} vmm_reply_create_t;

/* -------------------------------------------------------------------------
 * VMM_OP_DESTROY
 * ---------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t opcode;   /* VMM_OP_DESTROY */
    uint32_t slot_id;
} vmm_req_destroy_t;

typedef struct __attribute__((packed)) {
    uint32_t result;   /* 0 on success; vmm_error_t on failure */
} vmm_reply_destroy_t;

/* -------------------------------------------------------------------------
 * VMM_OP_START / STOP / PAUSE / RESUME — share the same minimal request
 * ---------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t opcode;   /* VMM_OP_START | STOP | PAUSE | RESUME */
    uint32_t slot_id;
} vmm_req_control_t;

typedef struct __attribute__((packed)) {
    uint32_t result;   /* 0 on success; vmm_error_t on failure */
} vmm_reply_control_t;

/* -------------------------------------------------------------------------
 * VMM_OP_INFO
 * ---------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t opcode;   /* VMM_OP_INFO */
    uint32_t slot_id;
} vmm_req_info_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint8_t  state;      /* vm_state_t cast to uint8_t */
    uint8_t  guest_type; /* guest_type_t cast to uint8_t */
    uint8_t  _pad[2];
    uint32_t ram_mb;
    uint32_t dev_mask;   /* GUEST_DEV_* bitmask of currently bound devices */
    uint8_t  label[32];
} vmm_reply_info_t;

/* -------------------------------------------------------------------------
 * VMM_OP_LIST — returns up to 8 active slot IDs per call.
 * To page through more than 8 slots, re-issue with offset incremented.
 * ---------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t opcode;   /* VMM_OP_LIST */
    uint32_t offset;   /* index of first slot to return; 0 for initial call */
} vmm_req_list_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t count;        /* number of valid entries in slot_ids[] */
    uint32_t slot_ids[8];
} vmm_reply_list_t;

/* -------------------------------------------------------------------------
 * VMM_OP_SNAPSHOT / RESTORE
 * ---------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t opcode;   /* VMM_OP_SNAPSHOT */
    uint32_t slot_id;
} vmm_req_snapshot_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t slot_id;
    uint8_t  snap_hash[32]; /* SHA-256 of snapshot blob written to block storage */
} vmm_reply_snapshot_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;        /* VMM_OP_RESTORE */
    uint32_t slot_id;
    uint8_t  snap_hash[32];
} vmm_req_restore_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
} vmm_reply_restore_t;

/* -------------------------------------------------------------------------
 * Error codes returned in result fields
 * ---------------------------------------------------------------------- */
typedef enum {
    VMM_OK              = 0,
    VMM_ERR_NO_SLOT     = 1,  /* all VMM slots in use */
    VMM_ERR_BAD_SLOT    = 2,  /* slot_id does not refer to an active slot */
    VMM_ERR_WRONG_STATE = 3,  /* operation not valid in the slot's current state */
    VMM_ERR_OOM         = 4,  /* insufficient memory to satisfy the request */
} vmm_error_t;

/* -------------------------------------------------------------------------
 * Invariants (enforced by each VMM PD implementation):
 *
 *  I1. All VMM PD source files (linux_vmm.c, freebsd_vmm.c, …) must
 *      include this header and use the opcode constants defined here.
 *      Per-VMM opcode definitions are forbidden.
 *
 *  I2. A VMM PD must complete the full guest binding protocol defined in
 *      guest_contract.h — including publishing EVENT_GUEST_READY — before
 *      transitioning a slot to VM_STATE_RUNNING.
 *
 *  I3. No VMM PD may access hardware directly.  All device I/O must go
 *      through the generic device PD handles recorded in guest_capabilities_t.
 *
 *  I4. slot_id values are assigned by the VMM PD and are unique within that
 *      PD.  They are not globally unique across PDs; callers must track which
 *      VMM PD owns a given slot_id.
 *
 *  I5. VMM_OP_SNAPSHOT blocks until the snapshot is durably written.  The
 *      snap_hash in vmm_reply_snapshot_t is the only durable identifier for
 *      a snapshot; callers are responsible for storing it.
 * ---------------------------------------------------------------------- */
