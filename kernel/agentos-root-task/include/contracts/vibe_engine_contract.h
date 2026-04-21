/*
 * VibeEngine IPC Contract
 *
 * The VibeEngine PD manages the hot-swap registry: loading, validating,
 * proposing, committing, and rolling back WASM service modules.
 *
 * Channel: CH_VIBEENGINE (see agentos.h)
 * Opcodes: OP_VIBE_* / MSG_VIBE_* (see agentos.h)
 *
 * Invariants:
 *   - VALIDATE must succeed before PROPOSE is called for a module.
 *   - PROPOSE returns a proposal_id; COMMIT or ROLLBACK_REQ must follow.
 *   - At most one proposal per swap slot may be in-flight at a time.
 *   - HOTRELOAD is a fast-path commit when layout/caps are compatible;
 *     on mismatch, it returns HOTRELOAD_FALLBACK (caller tears down slot).
 *   - REGISTRY_STATUS and REGISTRY_QUERY are read-only; they never mutate state.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define VIBEENGINE_CH_CONTROLLER  CH_VIBEENGINE  /* controller ↔ vibe_engine */

/* ─── Request structs ────────────────────────────────────────────────────── */

struct vibe_engine_req_validate {
    uint64_t module_hash_lo;    /* low 64 bits of module SHA-256 */
    uint64_t module_hash_hi;    /* high 64 bits of module SHA-256 */
};

struct vibe_engine_req_propose {
    uint32_t service_id;        /* which service slot to update */
    uint32_t flags;             /* PROPOSE_FLAG_* */
};

#define PROPOSE_FLAG_HOTRELOAD  (1u << 0)  /* attempt hot-reload path first */

struct vibe_engine_req_commit {
    uint32_t proposal_id;
};

struct vibe_engine_req_rollback {
    uint32_t proposal_id;
};

struct vibe_engine_req_replay {
    uint32_t flags;             /* REPLAY_FLAG_* */
};

#define REPLAY_FLAG_RESET_FIRST (1u << 0)  /* clear registry before replay */

struct vibe_engine_req_hotreload {
    uint32_t service_id;
    uint64_t new_hash_lo;
    uint64_t new_hash_hi;
};

struct vibe_engine_req_registry_status {
    /* no fields */
};

struct vibe_engine_req_registry_query {
    uint64_t module_hash_lo;
    uint64_t module_hash_hi;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct vibe_engine_reply_validate {
    uint32_t ok;                /* 0 = valid */
    uint32_t flags;             /* capability requirements bitmask */
};

struct vibe_engine_reply_propose {
    uint32_t ok;
    uint32_t proposal_id;
};

struct vibe_engine_reply_commit {
    uint32_t ok;
};

struct vibe_engine_reply_rollback {
    uint32_t ok;
};

struct vibe_engine_reply_replay {
    uint32_t ok;
    uint32_t entries_loaded;
};

struct vibe_engine_reply_hotreload {
    uint32_t result;            /* HOTRELOAD_OK / HOTRELOAD_FALLBACK / HOTRELOAD_ERR_CAPS */
};

struct vibe_engine_reply_registry_status {
    uint32_t total_entries;
    uint32_t active_slots;
    uint64_t bytes_used;
};

struct vibe_engine_reply_registry_query {
    uint32_t known;             /* 1 = in registry */
    uint32_t flags;             /* cap flags for this module */
    uint32_t use_count;
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum vibe_engine_error {
    VIBEENGINE_OK                = 0,
    VIBEENGINE_ERR_INVALID_HASH  = 1,  /* module hash unknown or corrupt */
    VIBEENGINE_ERR_CAP_DENIED    = 2,  /* module requests caps not granted */
    VIBEENGINE_ERR_SLOT_BUSY     = 3,  /* proposal already in-flight for slot */
    VIBEENGINE_ERR_BAD_PROPOSAL  = 4,  /* proposal_id not found */
    VIBEENGINE_ERR_ROLLBACK_FAIL = 5,  /* rollback could not restore prior state */
};
