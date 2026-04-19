/*
 * VibeSwap IPC Contract
 *
 * VibeSwap orchestrates zero-downtime hot-swap of WASM service modules.
 * The controller initiates a swap; VibeSwap coordinates with swap slots
 * via the SLOT_READY/SLOT_FAILED/SLOT_HEALTHY notification protocol.
 *
 * Channel: CH_VIBEENGINE (VibeSwap shares vibe_engine channel in current build)
 * Opcodes: MSG_VIBE_SWAP_* / MSG_VIBE_SLOT_* (see agentos.h)
 *
 * Invariants:
 *   - SWAP_BEGIN initiates the sequence; only one swap may be in flight per slot.
 *   - SWAP_ACTIVATE transitions the new module to live; the old module is freed.
 *   - SWAP_ROLLBACK reverts to the prior module; valid only after SWAP_BEGIN.
 *   - SWAP_HEALTH queries a slot's current health state (no side effects).
 *   - SWAP_STATUS returns global swap coordinator state.
 *   - The slot sends SLOT_READY / SLOT_FAILED / SLOT_HEALTHY asynchronously;
 *     these are notifications, not replies to PPC calls.
 */

#pragma once
#include "../agentos.h"

/* ─── Request structs ────────────────────────────────────────────────────── */

struct vibe_swap_req_begin {
    uint32_t slot_id;           /* swap slot index (0..MAX_SWAP_SLOTS-1) */
    uint64_t new_hash_lo;       /* new module WASM hash */
    uint64_t new_hash_hi;
    uint32_t flags;             /* SWAP_FLAG_* */
};

#define SWAP_FLAG_HEALTH_CHECK (1u << 0)  /* require SLOT_HEALTHY before ACTIVATE */

struct vibe_swap_req_activate {
    uint32_t slot_id;
};

struct vibe_swap_req_rollback {
    uint32_t slot_id;
};

struct vibe_swap_req_health {
    uint32_t slot_id;
};

struct vibe_swap_req_status {
    /* no fields */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct vibe_swap_reply_begin {
    uint32_t ok;
    uint32_t swap_id;           /* opaque handle for this swap operation */
};

struct vibe_swap_reply_activate {
    uint32_t ok;
};

struct vibe_swap_reply_rollback {
    uint32_t ok;
};

struct vibe_swap_reply_health {
    uint32_t ok;
    uint32_t state;             /* SWAP_SLOT_STATE_* */
};

#define SWAP_SLOT_STATE_EMPTY    0
#define SWAP_SLOT_STATE_LOADING  1
#define SWAP_SLOT_STATE_LIVE     2
#define SWAP_SLOT_STATE_FAILED   3
#define SWAP_SLOT_STATE_STANDBY  4

struct vibe_swap_reply_status {
    uint32_t in_flight;         /* number of swaps in progress */
    uint32_t total_swaps;       /* since boot */
    uint32_t total_rollbacks;
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum vibe_swap_error {
    VIBE_SWAP_OK              = 0,
    VIBE_SWAP_ERR_BUSY        = 1,  /* swap already in flight for slot */
    VIBE_SWAP_ERR_BAD_SLOT    = 2,
    VIBE_SWAP_ERR_LOAD_FAIL   = 3,  /* SLOT_FAILED received */
    VIBE_SWAP_ERR_NO_PRIOR    = 4,  /* ROLLBACK with no prior module */
};
