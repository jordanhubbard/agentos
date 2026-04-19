#pragma once
/* VIBE_SWAP contract — version 1
 * PD: vibe_swap (controller-side swap orchestration) | Source: src/vibe_engine.c
 * Channel: CH_CONTROLLER_SWAP_SLOT_N=30-33 (controller -> swap slots)
 */
#include <stdint.h>
#include <stdbool.h>

#define VIBE_SWAP_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
#define CH_SWAP_SLOT_0             30   /* cross-ref: agentos.h SWAP_SLOT_BASE_CH */
#define CH_SWAP_SLOT_1             31
#define CH_SWAP_SLOT_2             32
#define CH_SWAP_SLOT_3             33
#define SWAP_SLOT_COUNT             4

/* ── Opcodes (controller -> vibe_engine / swap slots) ── */
#define VIBE_SWAP_OP_BEGIN         0x0501u  /* VibeEngine -> Controller: initiate swap sequence */
#define VIBE_SWAP_OP_ACTIVATE      0x0502u  /* Controller -> slot: go live with new module */
#define VIBE_SWAP_OP_ROLLBACK      0x0503u  /* Controller: revert all slots to previous module */
#define VIBE_SWAP_OP_HEALTH        0x0504u  /* Controller -> slot: request health status */
#define VIBE_SWAP_OP_STATUS        0x0505u  /* Query current swap state machine state */

/* ── Swap state machine ── */
#define VIBE_SWAP_STATE_IDLE       0u  /* no swap in progress */
#define VIBE_SWAP_STATE_LOADING    1u  /* new module being loaded into slot */
#define VIBE_SWAP_STATE_ACTIVATING 2u  /* slots transitioning to new module */
#define VIBE_SWAP_STATE_LIVE       3u  /* new module fully active */
#define VIBE_SWAP_STATE_ROLLING    4u  /* rollback in progress */
#define VIBE_SWAP_STATE_FAILED     5u  /* swap failed; system in fallback */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VIBE_SWAP_OP_BEGIN */
    uint32_t service_id;      /* service to swap (maps to slot group) */
    uint64_t new_module_hash_lo;
    uint64_t new_module_hash_hi;
    uint32_t proposal_id;     /* from vibe_engine proposal */
    uint32_t flags;           /* VIBE_SWAP_FLAG_* */
} vibe_swap_req_begin_t;

#define VIBE_SWAP_FLAG_DRAIN_FIRST (1u << 0)  /* wait for in-flight requests to complete */
#define VIBE_SWAP_FLAG_ATOMIC      (1u << 1)  /* all slots must swap atomically */

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else vibe_swap_error_t */
    uint32_t swap_id;         /* opaque handle for this swap session */
} vibe_swap_reply_begin_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VIBE_SWAP_OP_ACTIVATE */
    uint32_t slot_id;         /* swap slot to activate (0-3) */
    uint32_t swap_id;
} vibe_swap_req_activate_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} vibe_swap_reply_activate_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VIBE_SWAP_OP_ROLLBACK */
    uint32_t swap_id;
    uint32_t reason;          /* vibe_swap_error_t describing why rollback was triggered */
} vibe_swap_req_rollback_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t slots_restored;  /* number of slots reverted */
} vibe_swap_reply_rollback_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VIBE_SWAP_OP_HEALTH */
    uint32_t slot_id;
    uint32_t swap_id;
} vibe_swap_req_health_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t health;          /* 0 = healthy */
    uint32_t response_ticks;  /* ticks to respond (latency indicator) */
} vibe_swap_reply_health_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VIBE_SWAP_OP_STATUS */
} vibe_swap_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t state;           /* VIBE_SWAP_STATE_* */
    uint32_t active_swap_id;  /* 0 if IDLE */
    uint32_t live_slots;      /* bitmask of slots in LIVE state */
} vibe_swap_reply_status_t;

/* ── Error codes ── */
typedef enum {
    VIBE_SWAP_OK              = 0,
    VIBE_SWAP_ERR_BUSY        = 1,  /* swap already in progress */
    VIBE_SWAP_ERR_BAD_SLOT    = 2,  /* slot_id out of range */
    VIBE_SWAP_ERR_LOAD_FAIL   = 3,  /* slot failed to load new module */
    VIBE_SWAP_ERR_HEALTH_FAIL = 4,  /* slot health check failed post-activate */
    VIBE_SWAP_ERR_NO_PREV     = 5,  /* rollback with no previous module available */
    VIBE_SWAP_ERR_BAD_ID      = 6,  /* swap_id does not match active session */
} vibe_swap_error_t;

/* ── Invariants ──
 * - Only one swap session may be in progress at a time.
 * - ACTIVATE must be called per slot; all slots must ACK before LIVE state.
 * - ROLLBACK is always safe: if no previous module, slots are shut down cleanly.
 * - VIBE_SWAP_FLAG_ATOMIC: any single slot failure triggers rollback of all slots.
 * - swap_id is unique per session and must be echoed in all subsequent operations.
 */
