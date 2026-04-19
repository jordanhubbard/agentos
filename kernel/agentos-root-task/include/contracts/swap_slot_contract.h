#pragma once
/* SWAP_SLOT contract — version 1
 * PD: swap_slot_N (N=0..3) | Source: src/swap_slot.c
 * Channel: SWAPSLOT_CH_CONTROLLER=0 (from swap slot perspective)
 * Controller sees slots at CH_CONTROLLER_SWAP_SLOT_N=30-33
 */
#include <stdint.h>
#include <stdbool.h>

#define SWAP_SLOT_CONTRACT_VERSION 1

/* ── Channel IDs ── */
/* From swap slot perspective */
#define SWAPSLOT_CH_CONTROLLER     0   /* cross-ref: agentos.h */

/* From controller perspective (cross-ref: agentos.h SWAP_SLOT_BASE_CH) */
#define CH_SWAP_SLOT_BASE          30  /* channels 30-33 are swap slot 0-3 */
#define CH_SWAP_SLOT_MAX_IDX        3  /* highest slot index */

/* ── Opcodes (swap slot -> controller notifications) ── */
#define SWAP_SLOT_OP_READY         0x0601u  /* slot loaded new module successfully */
#define SWAP_SLOT_OP_FAILED        0x0602u  /* slot failed to load new module */
#define SWAP_SLOT_OP_HEALTHY       0x0603u  /* slot responds to health check: ok */

/* ── Opcodes (controller -> swap slot commands) ── */
#define SWAP_SLOT_OP_LOAD          0x0610u  /* load a WASM module into this slot */
#define SWAP_SLOT_OP_ACTIVATE      0x0611u  /* go live: begin serving requests */
#define SWAP_SLOT_OP_DEACTIVATE    0x0612u  /* stop serving, drain in-flight requests */
#define SWAP_SLOT_OP_RESTORE       0x0613u  /* revert to previous module snapshot */
#define SWAP_SLOT_OP_QUERY         0x0614u  /* query current slot state */

/* ── Slot states ── */
#define SWAP_SLOT_STATE_EMPTY      0u  /* no module loaded */
#define SWAP_SLOT_STATE_LOADING    1u  /* load in progress */
#define SWAP_SLOT_STATE_STANDBY    2u  /* loaded, not yet active */
#define SWAP_SLOT_STATE_ACTIVE     3u  /* serving requests */
#define SWAP_SLOT_STATE_DRAINING   4u  /* deactivating, completing requests */
#define SWAP_SLOT_STATE_ERROR      5u  /* load or execution failed */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* SWAP_SLOT_OP_LOAD */
    uint64_t wasm_hash_lo;    /* module to load */
    uint64_t wasm_hash_hi;
    uint32_t staging_offset;  /* byte offset in staging MR where WASM resides */
    uint32_t staging_size;    /* byte size of WASM module */
    uint32_t cap_mask;        /* AGENTOS_CAP_* granted to this slot */
} swap_slot_req_load_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = load accepted (async; READY/FAILED notify follows) */
} swap_slot_reply_load_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* SWAP_SLOT_OP_ACTIVATE */
} swap_slot_req_activate_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} swap_slot_reply_activate_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* SWAP_SLOT_OP_DEACTIVATE */
    uint32_t drain_timeout_ms; /* max ms to wait for in-flight requests */
} swap_slot_req_deactivate_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t requests_drained; /* number of in-flight requests completed */
} swap_slot_reply_deactivate_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* SWAP_SLOT_OP_RESTORE */
} swap_slot_req_restore_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else SWAP_SLOT_ERR_NO_PREV */
    uint64_t restored_hash_lo;
    uint64_t restored_hash_hi;
} swap_slot_reply_restore_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* SWAP_SLOT_OP_QUERY */
} swap_slot_req_query_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t state;           /* SWAP_SLOT_STATE_* */
    uint64_t current_hash_lo;
    uint64_t current_hash_hi;
    uint64_t prev_hash_lo;    /* previous module hash (for rollback) */
    uint64_t prev_hash_hi;
} swap_slot_reply_query_t;

/* Notification sent by slot to controller on successful load */
typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* SWAP_SLOT_OP_READY */
    uint32_t slot_id;         /* self-reported slot index (0-3) */
    uint64_t wasm_hash_lo;
    uint64_t wasm_hash_hi;
} swap_slot_notify_ready_t;

/* Notification sent by slot to controller on load failure */
typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* SWAP_SLOT_OP_FAILED */
    uint32_t slot_id;
    uint32_t error_code;      /* swap_slot_error_t */
} swap_slot_notify_failed_t;

/* ── Error codes ── */
typedef enum {
    SWAP_SLOT_OK              = 0,
    SWAP_SLOT_ERR_BAD_STATE   = 1,  /* command invalid in current state */
    SWAP_SLOT_ERR_LOAD_FAIL   = 2,  /* WASM validation or instantiation failed */
    SWAP_SLOT_ERR_NO_MEM      = 3,  /* insufficient memory to load module */
    SWAP_SLOT_ERR_NO_PREV     = 4,  /* restore with no previous module */
    SWAP_SLOT_ERR_TIMEOUT     = 5,  /* drain timed out */
} swap_slot_error_t;

/* ── Invariants ──
 * - A slot can only ACTIVATE from STANDBY state.
 * - LOAD is asynchronous; completion is signaled via READY or FAILED notification.
 * - RESTORE is synchronous and only valid from STANDBY or ACTIVE state.
 * - Each slot retains at most one previous module snapshot for rollback.
 * - Staging MR must be mapped and WASM valid before LOAD is called.
 */
