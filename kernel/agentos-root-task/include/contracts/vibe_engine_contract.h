#pragma once
/* VIBE_ENGINE contract — version 1
 * PD: vibe_engine | Source: src/vibe_engine.c | Channel: CH_VIBEENGINE=40 (from controller)
 */
#include <stdint.h>
#include <stdbool.h>

#define VIBE_ENGINE_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
#define CH_VIBEENGINE              40   /* controller <-> vibe_engine; cross-ref: agentos.h */

/* ── Opcodes ── */
#define VIBE_ENGINE_OP_PROPOSE          0x40u  /* propose a new WASM module for a service slot */
#define VIBE_ENGINE_OP_VALIDATE         0x41u  /* validate WASM module in staging region */
#define VIBE_ENGINE_OP_EXECUTE          0x42u  /* commit validated module to active slot */
#define VIBE_ENGINE_OP_STATUS           0x43u  /* query engine state and active slot info */
#define VIBE_ENGINE_OP_ROLLBACK         0x44u  /* revert active slot to previous module */
#define VIBE_ENGINE_OP_HEALTH           0x45u  /* query health of a running slot */
#define VIBE_ENGINE_OP_REPLAY           0x46u  /* boot replay: seed registry from AgentFS */
#define VIBE_ENGINE_OP_HOTRELOAD        0x47u  /* zero-downtime slot update */
#define VIBE_ENGINE_OP_REGISTRY_STATUS  0x48u  /* return total registry entries + stats */
#define VIBE_ENGINE_OP_REGISTRY_QUERY   0x4Bu  /* query registry by hash: known? flags? */

/* ── Hot-reload return codes (MR0) ── */
#define VIBE_HOTRELOAD_OK           0x00u  /* hot-reload succeeded */
#define VIBE_HOTRELOAD_FALLBACK     0x01u  /* layout/caps mismatch — fall back to teardown+respawn */
#define VIBE_HOTRELOAD_ERR_CAPS     0x02u  /* new module requests caps not in slot's grants */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VIBE_ENGINE_OP_PROPOSE */
    uint32_t service_id;      /* target service slot ID */
    uint32_t proposal_id;     /* caller-assigned monotonic ID */
    uint32_t wasm_offset;     /* byte offset into staging region */
    uint32_t wasm_size;       /* byte size of WASM module */
    uint64_t wasm_hash_lo;    /* lower 64 bits of expected content hash */
    uint64_t wasm_hash_hi;    /* upper 64 bits of expected content hash */
} vibe_engine_req_propose_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else vibe_engine_error_t */
    uint32_t proposal_id;     /* echoed back */
} vibe_engine_reply_propose_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VIBE_ENGINE_OP_VALIDATE */
    uint32_t proposal_id;     /* proposal to validate */
} vibe_engine_req_validate_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else vibe_engine_error_t */
    uint32_t cap_mask;        /* capability mask declared by module */
    uint32_t section_count;   /* number of WASM sections parsed */
} vibe_engine_reply_validate_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VIBE_ENGINE_OP_EXECUTE */
    uint32_t proposal_id;     /* validated proposal to commit */
    uint32_t flags;           /* VIBE_EXEC_FLAG_* bitmask */
} vibe_engine_req_execute_t;

#define VIBE_EXEC_FLAG_HOTRELOAD   (1u << 0)  /* attempt in-place hot-reload first */
#define VIBE_EXEC_FLAG_FORCE       (1u << 1)  /* force teardown+respawn even if hotreload possible */

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t slot_id;         /* slot now running new module */
    uint32_t hotreload_result;/* VIBE_HOTRELOAD_* code */
} vibe_engine_reply_execute_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VIBE_ENGINE_OP_STATUS */
    uint32_t slot_id;         /* 0xFFFFFFFF = query engine global state */
} vibe_engine_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t active_slots;    /* number of currently active swap slots */
    uint32_t pending_proposals; /* proposals awaiting validation */
    uint32_t registry_entries;  /* modules in registry */
    uint64_t current_hash_lo; /* hash of module in queried slot (0 if global) */
    uint64_t current_hash_hi;
} vibe_engine_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VIBE_ENGINE_OP_ROLLBACK */
    uint32_t slot_id;         /* slot to roll back */
} vibe_engine_req_rollback_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint64_t restored_hash_lo;
    uint64_t restored_hash_hi;
} vibe_engine_reply_rollback_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VIBE_ENGINE_OP_HEALTH */
    uint32_t slot_id;
} vibe_engine_req_health_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t health;          /* 0 = healthy, else vibe_engine_error_t */
    uint32_t heartbeat_ticks; /* ticks since last slot heartbeat */
} vibe_engine_reply_health_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* VIBE_ENGINE_OP_REGISTRY_QUERY */
    uint64_t wasm_hash_lo;
    uint64_t wasm_hash_hi;
} vibe_engine_req_registry_query_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = found, VIBE_ENGINE_ERR_NOT_FOUND = unknown */
    uint32_t flags;           /* registry flags: validated, trusted, etc. */
    uint32_t slot_id;         /* slot currently running this module (0xFF = none) */
} vibe_engine_reply_registry_query_t;

/* ── Error codes ── */
typedef enum {
    VIBE_ENGINE_OK                = 0,
    VIBE_ENGINE_ERR_BAD_WASM      = 1,  /* module failed WASM validation */
    VIBE_ENGINE_ERR_BAD_CAPS      = 2,  /* module requests disallowed capabilities */
    VIBE_ENGINE_ERR_NO_SLOT       = 3,  /* no free swap slot available */
    VIBE_ENGINE_ERR_NOT_FOUND     = 4,  /* proposal_id or hash not in registry */
    VIBE_ENGINE_ERR_BUSY          = 5,  /* slot currently in transition */
    VIBE_ENGINE_ERR_STAGING       = 6,  /* staging region not mapped or corrupt */
    VIBE_ENGINE_ERR_HASH_MISMATCH = 7,  /* content hash does not match header */
    VIBE_ENGINE_ERR_NO_PREVIOUS   = 8,  /* rollback requested but no previous module */
} vibe_engine_error_t;

/* ── Invariants ──
 * - OP_PROPOSE must precede OP_VALIDATE which must precede OP_EXECUTE.
 * - Staging region (4MB MR, last 64 bytes = VIBE_META) must be mapped before PROPOSE.
 * - Only one proposal per service_id may be in-flight at a time.
 * - ROLLBACK is only valid if a previous module snapshot exists in the registry.
 * - Registry is seeded at boot via OP_REPLAY before any agent proposes modules.
 */
