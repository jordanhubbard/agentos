/*
 * AgentPool IPC Contract
 *
 * AgentPool manages the pool of Worker PDs.  Callers allocate a worker slot,
 * assign a task, and free the slot when done.
 *
 * Channel: CH_AGENT_POOL (see agentos.h)
 * Opcodes: MSG_AGENTPOOL_* (see agentos.h)
 *
 * Invariants:
 *   - ALLOC_WORKER returns a slot only if a worker is idle.
 *   - A caller that holds a slot must eventually call FREE_WORKER.
 *   - STATUS is read-only and does not affect allocation state.
 *   - cap_mask limits what the allocated worker can do; it cannot exceed
 *     the cap_mask granted to the caller at spawn time.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define AGENTPOOL_CH_CONTROLLER  CH_AGENT_POOL

/* ─── Request structs ────────────────────────────────────────────────────── */

struct agentpool_req_alloc_worker {
    uint32_t cap_mask;          /* AGENTOS_CAP_* bitmask for this worker */
    uint32_t priority;          /* agentos_priority_t hint */
};

struct agentpool_req_free_worker {
    uint32_t worker_slot;       /* slot returned by ALLOC_WORKER */
};

struct agentpool_req_status {
    /* no fields */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct agentpool_reply_alloc_worker {
    uint32_t ok;
    uint32_t worker_slot;       /* WORKER_POOL_BASE_CH offset for this slot */
};

struct agentpool_reply_free_worker {
    uint32_t ok;
};

struct agentpool_reply_status {
    uint32_t total;             /* WORKER_POOL_SIZE */
    uint32_t busy;
    uint32_t idle;
    uint32_t faulted;
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum agentpool_error {
    AGENTPOOL_OK              = 0,
    AGENTPOOL_ERR_EXHAUSTED   = 1,  /* no idle workers available */
    AGENTPOOL_ERR_BAD_SLOT    = 2,  /* worker_slot out of range */
    AGENTPOOL_ERR_CAP_EXCEED  = 3,  /* requested cap_mask > caller's cap_mask */
    AGENTPOOL_ERR_NOT_OWNED   = 4,  /* caller does not own this slot */
};
