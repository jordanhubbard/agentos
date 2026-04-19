#pragma once
/* MESH_AGENT contract — version 1
 * PD: mesh_agent | Source: src/mesh_agent.c | Channel: (routed via EventBus; no fixed PPC channel)
 */
#include <stdint.h>
#include <stdbool.h>

#define MESH_AGENT_CONTRACT_VERSION 1

/* ── Opcodes ── */
#define MESH_AGENT_OP_ANNOUNCE       0x0A01u  /* node registration: node_id, slot_count, gpu_slots */
#define MESH_AGENT_OP_ANNOUNCE_REPLY 0x0A02u
#define MESH_AGENT_OP_STATUS         0x0A03u  /* query: number of known peers, total slots */
#define MESH_AGENT_OP_STATUS_REPLY   0x0A04u
#define MESH_AGENT_OP_REMOTE_SPAWN   0x0A05u  /* spawn agent on best-available peer node */
#define MESH_AGENT_OP_REMOTE_SPAWN_REPLY 0x0A06u  /* reply: node_id + ticket_id, or local fallback */
#define MESH_AGENT_OP_HEARTBEAT      0x0A07u  /* periodic liveness ping from peer */
#define MESH_AGENT_OP_PEER_DOWN      0x0A08u  /* EventBus: peer went offline */

/* ── Node capability flags ── */
#define MESH_NODE_FLAG_GPU          (1u << 0)  /* node has GPU slots */
#define MESH_NODE_FLAG_STORAGE      (1u << 1)  /* node has persistent storage */
#define MESH_NODE_FLAG_LEADER       (1u << 2)  /* node is current mesh coordinator */
#define MESH_NODE_FLAG_REACHABLE    (1u << 3)  /* node currently responding to heartbeat */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* MESH_AGENT_OP_ANNOUNCE */
    uint32_t node_id;         /* unique node identifier */
    uint32_t slot_count;      /* total worker slots on this node */
    uint32_t gpu_slots;       /* GPU execution slots available */
    uint32_t flags;           /* MESH_NODE_FLAG_* */
    uint8_t  node_addr[16];   /* node network address (IPv6 or IPv4-mapped) */
} mesh_agent_req_announce_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else mesh_agent_error_t */
    uint32_t peer_count;      /* number of peers already known to mesh */
    uint32_t assigned_id;     /* confirmed or reassigned node_id */
} mesh_agent_reply_announce_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* MESH_AGENT_OP_STATUS */
} mesh_agent_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t peer_count;      /* number of known live peers */
    uint32_t total_slots;     /* aggregate worker slots across all peers */
    uint32_t total_gpu_slots;
    uint32_t local_node_id;
} mesh_agent_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* MESH_AGENT_OP_REMOTE_SPAWN */
    uint64_t wasm_hash_lo;    /* agent module to spawn */
    uint64_t wasm_hash_hi;
    uint32_t cap_mask;        /* AGENTOS_CAP_* requested */
    uint32_t priority;        /* scheduling priority */
    uint32_t preferred_node;  /* 0 = best-available, else specific node_id */
} mesh_agent_req_remote_spawn_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t target_node_id;  /* node where agent was spawned */
    uint32_t ticket_id;       /* spawn ticket for tracking */
    uint32_t local_fallback;  /* 1 if spawned locally due to no peer capacity */
} mesh_agent_reply_remote_spawn_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* MESH_AGENT_OP_HEARTBEAT */
    uint32_t sender_node_id;
    uint64_t timestamp_ns;
    uint32_t free_slots;      /* current free worker slots on sender */
} mesh_agent_req_heartbeat_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} mesh_agent_reply_heartbeat_t;

/* EventBus payload for MESH_AGENT_OP_PEER_DOWN */
typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* MESH_AGENT_OP_PEER_DOWN */
    uint32_t node_id;         /* peer that went offline */
    uint32_t last_seen_ticks; /* ticks since last heartbeat */
} mesh_agent_event_peer_down_t;

/* ── Error codes ── */
typedef enum {
    MESH_AGENT_OK             = 0,
    MESH_AGENT_ERR_DUPLICATE  = 1,  /* node_id already registered */
    MESH_AGENT_ERR_FULL       = 2,  /* peer table full */
    MESH_AGENT_ERR_NO_PEER    = 3,  /* no peer with sufficient capacity */
    MESH_AGENT_ERR_BAD_NODE   = 4,  /* node_id not in peer table */
    MESH_AGENT_ERR_SPAWN_FAIL = 5,  /* remote spawn request rejected by peer */
} mesh_agent_error_t;

/* ── Invariants ──
 * - ANNOUNCE must be called once per node on join; duplicate node_ids are rejected.
 * - Heartbeat interval must be less than the peer-down timeout (platform-defined).
 * - REMOTE_SPAWN falls back to local execution if no peer has free capacity.
 * - PEER_DOWN is an EventBus event, not a PPC reply; subscribers must handle it.
 * - Mesh membership is soft-state: nodes that miss heartbeats are marked unreachable.
 */
