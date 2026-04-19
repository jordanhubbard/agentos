#pragma once
/* AGENT_POOL contract — version 1
 * PD: agent_pool | Source: src/agent_pool.c | Channel: WORKER_POOL_BASE_CH=20 (from controller)
 */
#include <stdint.h>
#include <stdbool.h>

#define AGENT_POOL_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
/* Worker pool occupies channels 20-27 (WORKER_POOL_BASE_CH + index) */
#define CH_AGENT_POOL_BASE     20   /* first channel of 8-slot worker pool; cross-ref: agentos.h */
#define AGENT_POOL_SIZE         8   /* maximum concurrent workers */

/* ── Opcodes (0xAA00 range) ── */
#define AGENT_POOL_OP_ALLOC_WORKER  0xAA01u  /* allocate a worker slot from the pool */
#define AGENT_POOL_OP_FREE_WORKER   0xAA02u  /* return a worker slot to the pool */
#define AGENT_POOL_OP_STATUS        0xAA03u  /* query pool occupancy and slot states */

/* ── Worker slot states ── */
#define AGENT_POOL_SLOT_FREE        0u  /* slot available for allocation */
#define AGENT_POOL_SLOT_BUSY        1u  /* slot running an agent */
#define AGENT_POOL_SLOT_DRAINING    2u  /* slot completing work, will free */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* AGENT_POOL_OP_ALLOC_WORKER */
    uint32_t priority;        /* requested scheduling priority (agentos_priority_t) */
    uint32_t cap_mask;        /* AGENTOS_CAP_* mask for the new worker */
    uint64_t agent_id_hi;     /* agent ID to associate with this slot */
    uint64_t agent_id_lo;
} agent_pool_req_alloc_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else agent_pool_error_t */
    uint32_t slot_id;         /* 0-7: allocated worker slot index */
    uint32_t channel_id;      /* seL4 channel for this slot (WORKER_POOL_BASE_CH + slot_id) */
} agent_pool_reply_alloc_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* AGENT_POOL_OP_FREE_WORKER */
    uint32_t slot_id;         /* slot to release */
} agent_pool_req_free_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} agent_pool_reply_free_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* AGENT_POOL_OP_STATUS */
} agent_pool_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t free_slots;      /* number of unoccupied slots */
    uint32_t busy_slots;      /* number of slots running agents */
    uint8_t  slot_states[8];  /* per-slot state: AGENT_POOL_SLOT_* */
} agent_pool_reply_status_t;

/* ── Error codes ── */
typedef enum {
    AGENT_POOL_OK             = 0,
    AGENT_POOL_ERR_FULL       = 1,  /* all 8 worker slots occupied */
    AGENT_POOL_ERR_BAD_SLOT   = 2,  /* slot_id out of range or not allocated */
    AGENT_POOL_ERR_NOT_FREE   = 3,  /* free called on slot not in BUSY state */
    AGENT_POOL_ERR_BAD_CAPS   = 4,  /* requested cap_mask not permitted */
} agent_pool_error_t;

/* ── Invariants ──
 * - Exactly AGENT_POOL_SIZE (8) worker slots exist; indices are 0-7.
 * - Each allocated slot maps to seL4 channel (WORKER_POOL_BASE_CH + slot_id).
 * - cap_mask must be a subset of caps granted to the agent pool by the root task.
 * - FREE must be called before a slot can be reallocated.
 * - agent_id must be unique among currently allocated slots.
 */
