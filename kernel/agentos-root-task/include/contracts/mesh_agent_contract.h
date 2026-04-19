/*
 * MeshAgent IPC Contract
 *
 * The MeshAgent PD manages the distributed node mesh: peer discovery,
 * heartbeating, and remote agent spawn.
 *
 * Channel: (controller ↔ mesh_agent; channel assigned in agentos.system)
 * Opcodes: MSG_MESH_* / MSG_REMOTE_SPAWN_* (see agentos.h)
 *
 * Invariants:
 *   - MSG_MESH_ANNOUNCE must be sent by each node at boot before it receives
 *     remote spawn requests.
 *   - MSG_MESH_HEARTBEAT must be sent by each peer at least once per
 *     MESH_HEARTBEAT_INTERVAL_MS; silence triggers MSG_MESH_PEER_DOWN.
 *   - MSG_REMOTE_SPAWN returns a ticket_id; completion is via EventBus.
 *   - MSG_MESH_PEER_DOWN is an EventBus notification, not a PPC reply.
 *   - node_id 0 is reserved for the local node.
 */

#pragma once
#include "../agentos.h"

/* ─── Request structs ────────────────────────────────────────────────────── */

struct mesh_agent_req_announce {
    uint32_t node_id;           /* this node's mesh ID */
    uint32_t slot_count;        /* available WASM worker slots */
    uint32_t gpu_slots;         /* available GPU slots */
    uint32_t flags;             /* MESH_NODE_FLAG_* */
};

#define MESH_NODE_FLAG_GPU    (1u << 0)  /* node has GPU capability */
#define MESH_NODE_FLAG_RELAY  (1u << 1)  /* node acts as mesh relay */

struct mesh_agent_req_status {
    /* no fields */
};

struct mesh_agent_req_remote_spawn {
    uint64_t wasm_hash_lo;
    uint64_t wasm_hash_hi;
    uint32_t cap_mask;
    uint32_t preferred_node;    /* 0 = best-available selection */
};

struct mesh_agent_req_heartbeat {
    uint32_t node_id;
    uint32_t tick;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct mesh_agent_reply_announce {
    uint32_t ok;
    uint32_t peer_count;        /* known peers at time of reply */
};

struct mesh_agent_reply_status {
    uint32_t peer_count;
    uint32_t total_slots;
    uint32_t available_slots;
};

struct mesh_agent_reply_remote_spawn {
    uint32_t ok;
    uint32_t node_id;           /* node that accepted the task */
    uint32_t ticket_id;         /* completion ticket */
};

struct mesh_agent_reply_heartbeat {
    uint32_t ok;
};

/* ─── Configuration ──────────────────────────────────────────────────────── */

#define MESH_HEARTBEAT_INTERVAL_MS  1000u
#define MESH_PEER_TIMEOUT_MS        5000u  /* peer declared down after this silence */

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum mesh_agent_error {
    MESH_OK                  = 0,
    MESH_ERR_ALREADY_ANNOUNCED = 1,
    MESH_ERR_NO_PEERS        = 2,
    MESH_ERR_NO_SLOTS        = 3,
    MESH_ERR_BAD_NODE        = 4,
};
