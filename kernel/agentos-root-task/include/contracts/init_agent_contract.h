#pragma once
/* INIT_AGENT contract — version 1
 * PD: init_agent | Source: src/init_agent.c | Channel: INITAGENT_CH_MONITOR=1, INITAGENT_CH_EVENTBUS=2 (from controller)
 */
#include <stdint.h>
#include <stdbool.h>

#define INIT_AGENT_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
#define CH_INIT_AGENT              1   /* controller -> init_agent (from controller: CH_CONTROLLER_INIT_AGENT) */
#define INITAGENT_CH_MONITOR       1   /* cross-ref: agentos.h */
#define INITAGENT_CH_EVENTBUS      2   /* cross-ref: agentos.h */

/* ── Opcodes ── */
#define INITAGENT_OP_START         0x0201u  /* controller -> init_agent: begin startup sequence */
#define INITAGENT_OP_SHUTDOWN      0x0202u  /* controller -> init_agent: begin orderly shutdown */
#define INITAGENT_OP_READY         0x0301u  /* init_agent -> controller: startup complete */
#define INITAGENT_OP_STATUS        0x0302u  /* init_agent -> controller: status report */
#define INITAGENT_OP_SPAWN_AGENT   0x0801u  /* caller -> init_agent: spawn WASM agent by hash */
#define INITAGENT_OP_SPAWN_REPLY   0x0802u  /* init_agent -> caller: agent_id or error */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* INITAGENT_OP_START */
    uint32_t flags;           /* reserved, must be 0 */
} init_agent_req_start_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} init_agent_reply_start_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* INITAGENT_OP_SHUTDOWN */
    uint32_t grace_ms;        /* milliseconds to allow for orderly teardown */
} init_agent_req_shutdown_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} init_agent_reply_shutdown_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* INITAGENT_OP_STATUS */
} init_agent_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t state;           /* init_agent_state_t value */
    uint32_t agents_running;  /* number of active agent slots */
    uint32_t uptime_ticks;    /* ticks since init_agent started */
} init_agent_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* INITAGENT_OP_SPAWN_AGENT */
    uint64_t wasm_hash_lo;    /* lower 64 bits of WASM module content hash */
    uint64_t wasm_hash_hi;    /* upper 64 bits of WASM module content hash */
    uint32_t cap_mask;        /* requested AGENTOS_CAP_* bitmask */
    uint32_t priority;        /* scheduling priority (agentos_priority_t) */
} init_agent_req_spawn_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else init_agent_error_t */
    uint64_t agent_id_hi;     /* upper 64 bits of assigned agent ID */
    uint64_t agent_id_lo;     /* lower 64 bits of assigned agent ID */
    uint32_t slot_id;         /* worker slot assigned to this agent */
} init_agent_reply_spawn_t;

/* ── Agent lifecycle states ── */
typedef enum {
    INITAGENT_STATE_INIT       = 0,  /* pre-start */
    INITAGENT_STATE_RUNNING    = 1,  /* active, accepting spawn requests */
    INITAGENT_STATE_SHUTDOWN   = 2,  /* draining, no new spawns */
    INITAGENT_STATE_STOPPED    = 3,  /* fully stopped */
} init_agent_state_t;

/* ── Error codes ── */
typedef enum {
    INIT_AGENT_OK              = 0,
    INIT_AGENT_ERR_NOT_READY   = 1,  /* called before START completed */
    INIT_AGENT_ERR_SHUTDOWN    = 2,  /* system is shutting down */
    INIT_AGENT_ERR_NO_SLOT     = 3,  /* no free worker slot available */
    INIT_AGENT_ERR_BAD_HASH    = 4,  /* WASM hash not found in AgentFS */
    INIT_AGENT_ERR_BAD_CAPS    = 5,  /* requested capabilities not permitted */
    INIT_AGENT_ERR_LOAD_FAIL   = 6,  /* WASM load/validation failed */
} init_agent_error_t;

/* ── Invariants ──
 * - INITAGENT_OP_START must be called exactly once before any other operation.
 * - Spawn requests are rejected once INITAGENT_OP_SHUTDOWN has been issued.
 * - cap_mask must be a subset of the caps granted to init_agent by the root task.
 * - wasm_hash must identify a module already stored in AgentFS.
 */
