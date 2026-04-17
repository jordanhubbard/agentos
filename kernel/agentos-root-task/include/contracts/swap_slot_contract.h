/*
 * SwapSlot IPC Contract
 *
 * A SwapSlot PD hosts a single WASM service module.  The controller (via
 * VibeSwap) loads a module into a slot, activates it, and rolls it back.
 *
 * Channel: SWAPSLOT_CH_CONTROLLER (from slot perspective; see agentos.h)
 * Opcodes: MSG_VIBE_SLOT_* (see agentos.h)
 *
 * Invariants:
 *   - SLOT_READY is sent by the slot to the controller after successful load.
 *   - SLOT_FAILED is sent by the slot to the controller on load failure.
 *   - SLOT_HEALTHY is sent by the slot to the controller after a health check.
 *   - All three are notifications (not PPC replies); channel SWAPSLOT_CH_CONTROLLER.
 *   - The slot does not initiate communication; it only responds to notifications
 *     from the controller and sends the three status notifications above.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
/* SWAPSLOT_CH_CONTROLLER = 0 (from swap slot perspective; see agentos.h) */
/* From controller perspective: SWAP_SLOT_BASE_CH + slot_index             */

/* ─── Notification structs (slot → controller, via microkit_notify) ───────── */

struct swap_slot_notify_ready {
    uint32_t slot_id;
    uint64_t loaded_hash_lo;
    uint64_t loaded_hash_hi;
};

struct swap_slot_notify_failed {
    uint32_t slot_id;
    uint32_t error_code;        /* SWAP_SLOT_ERR_* */
};

struct swap_slot_notify_healthy {
    uint32_t slot_id;
    uint32_t checks_passed;
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum swap_slot_error {
    SWAP_SLOT_OK              = 0,
    SWAP_SLOT_ERR_VERIFY_FAIL = 1,  /* Ed25519/SHA-256 verification failed */
    SWAP_SLOT_ERR_BAD_WASM    = 2,  /* WASM structure validation failed */
    SWAP_SLOT_ERR_CAP_DENIED  = 3,  /* module capability requirements rejected */
    SWAP_SLOT_ERR_OOM         = 4,  /* insufficient memory for module */
};
