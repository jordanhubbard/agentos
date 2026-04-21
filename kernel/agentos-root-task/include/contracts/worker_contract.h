/*
 * Worker IPC Contract
 *
 * Worker PDs execute WASM agent tasks assigned by the controller.
 * The controller communicates with workers via dedicated channels in the
 * WORKER_POOL_BASE_CH..WORKER_POOL_BASE_CH+WORKER_POOL_SIZE-1 range.
 *
 * Channels: WORKER_POOL_BASE_CH + slot_index (see agentos.h)
 * Opcodes: MSG_WORKER_* (see agentos.h)
 *
 * Invariants:
 *   - A worker in IDLE state accepts ASSIGN but rejects RETRIEVE.
 *   - A worker in BUSY state accepts RETRIEVE and REVOKE but rejects ASSIGN.
 *   - REVOKE causes the in-progress task to abort; resources are freed.
 *   - STATUS is always safe to call regardless of worker state.
 *   - Worker replies to ASSIGN only after the task completes (synchronous).
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
/* Channel = WORKER_POOL_BASE_CH + slot (0..WORKER_POOL_SIZE-1) */

/* ─── Request structs ────────────────────────────────────────────────────── */

struct worker_req_assign {
    uint32_t task_id;           /* unique task identifier from controller */
    uint32_t cap_mask;          /* capabilities granted for this task */
    uint64_t wasm_hash_lo;      /* WASM module to load (low 64 bits) */
    uint64_t wasm_hash_hi;      /* WASM module to load (high 64 bits) */
};

struct worker_req_revoke {
    uint32_t task_id;           /* task to abort */
};

struct worker_req_retrieve {
    /* MSG_WORKER_RETRIEVE: worker pulls AgentFS object for its current task */
    uint32_t inode;
    uint32_t offset;
    uint32_t len;
};

struct worker_req_status {
    /* no fields */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct worker_reply_assign {
    uint32_t ok;
    uint32_t exit_code;         /* WASM exit code */
};

struct worker_reply_revoke {
    uint32_t ok;
};

struct worker_reply_retrieve {
    uint32_t ok;
    uint32_t actual;            /* bytes returned in shmem */
};

struct worker_reply_status {
    uint32_t state;             /* WORKER_STATE_* */
    uint32_t task_id;           /* current task (0 if idle) */
    uint32_t progress_pct;      /* 0-100 */
};

#define WORKER_STATE_IDLE     0
#define WORKER_STATE_BUSY     1
#define WORKER_STATE_FAULTED  2
#define WORKER_STATE_REVOKED  3

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum worker_error {
    WORKER_OK               = 0,
    WORKER_ERR_BUSY         = 1,  /* ASSIGN on already-busy worker */
    WORKER_ERR_IDLE         = 2,  /* RETRIEVE/REVOKE on idle worker */
    WORKER_ERR_BAD_TASK     = 3,  /* task_id mismatch */
    WORKER_ERR_WASM_LOAD    = 4,  /* WASM module load failed */
    WORKER_ERR_CAP_DENIED   = 5,  /* module requires caps not in cap_mask */
};
