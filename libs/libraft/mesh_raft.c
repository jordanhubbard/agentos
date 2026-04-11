/*
 * mesh_raft.c — Raft integration for agentOS mesh_agent
 *
 * Wires raft.c into the mesh agent loop:
 *   - Serialises RaftMsg to/from a compact wire format
 *   - Sends outgoing RPCs via an agentOS MsgBus topic
 *       "raft.<node_id>.<msg_type>"  (routed by the SquirrelBus)
 *   - Receives incoming RPCs via mesh_raft_on_msg() called by the
 *     mesh_agent's message-dispatch loop after deserialisation
 *   - Calls raft_tick() every ~10 ms from the agent heartbeat
 *   - On commit: updates the in-memory GPU task queue
 *   - Exposes mesh_raft_is_leader() for the GPU scheduler guard
 *
 * Wire format (little-endian, fixed-size, 96 bytes):
 *   [0]   uint8_t   msg_type      (RaftMsgType)
 *   [1]   uint8_t   from_node
 *   [2..7]  pad
 *   [8]   uint64_t  term
 *   [16]  -- union payload (80 bytes) --
 *
 *   RequestVote payload (at offset 16):
 *     uint64_t last_log_index
 *     uint64_t last_log_term
 *
 *   RequestVoteResponse payload:
 *     uint8_t  vote_granted
 *
 *   AppendEntries payload:
 *     uint64_t prev_log_index
 *     uint64_t prev_log_term
 *     uint64_t leader_commit
 *     uint8_t  entry_count
 *     RaftLogEntry entries[entry_count]   (each entry = 73 bytes)
 *
 *   AppendEntriesResponse payload:
 *     uint8_t  success
 *     uint64_t match_index
 *
 * Node IDs:
 *   0 = rocky   (do-host1, 146.190.134.110)
 *   1 = natasha (sparky GB10)
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#include "raft.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Wire format constants ───────────────────────────────────────────── */

#define WIRE_HEADER_SIZE     16   /* type(1) + from(1) + pad(6) + term(8) */
#define WIRE_MAX_SIZE       128   /* enough for AppendEntries with 8 entries */

/* Byte offsets within the wire buffer */
#define WIRE_OFF_TYPE        0
#define WIRE_OFF_FROM        1
#define WIRE_OFF_TERM        8
#define WIRE_OFF_PAYLOAD    16

/* ── MsgBus topic transport callback ────────────────────────────────── */

/*
 * Pluggable send function: caller sets this before calling mesh_raft_init()
 * if it wants a real transport (e.g. TCP, HTTP SquirrelBus, seL4 IPC).
 * Default implementation just logs and is safe to call in unit tests.
 *
 * Signature:  void fn(void *ud, const char *topic,
 *                     const uint8_t *buf, size_t len)
 */
typedef void (*MeshSendFn)(void *ud, const char *topic,
                            const uint8_t *buf, size_t len);

static MeshSendFn g_send_fn  = NULL;
static void      *g_send_ud  = NULL;

/*
 * Register a transport-level send function.
 * Call this before mesh_raft_init() to plug in a real transport.
 */
void mesh_raft_set_transport(MeshSendFn fn, void *userdata) {
    g_send_fn = fn;
    g_send_ud = userdata;
}

/* ── Serialisation helpers ───────────────────────────────────────────── */

static void write_u64_le(uint8_t *buf, uint64_t v) {
    for (int i = 0; i < 8; i++) { buf[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}

static uint64_t read_u64_le(const uint8_t *buf) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) { v = (v << 8) | buf[i]; }
    return v;
}

/*
 * Serialise a RaftMsg into a wire buffer.
 * Returns the number of bytes written, or 0 on error.
 */
static size_t raft_msg_serialise(const RaftMsg *msg, uint8_t *buf, size_t buf_size) {
    if (buf_size < WIRE_HEADER_SIZE) return 0;

    memset(buf, 0, WIRE_HEADER_SIZE);
    buf[WIRE_OFF_TYPE] = (uint8_t)msg->type;
    buf[WIRE_OFF_FROM] = msg->from_node;
    write_u64_le(buf + WIRE_OFF_TERM, msg->term);

    uint8_t *p = buf + WIRE_OFF_PAYLOAD;
    size_t remaining = buf_size - WIRE_OFF_PAYLOAD;

    switch (msg->type) {
        case RAFT_MSG_REQUEST_VOTE:
            if (remaining < 16) return 0;
            write_u64_le(p,     msg->u.request_vote.last_log_index);
            write_u64_le(p + 8, msg->u.request_vote.last_log_term);
            return WIRE_OFF_PAYLOAD + 16;

        case RAFT_MSG_REQUEST_VOTE_RESP:
            if (remaining < 1) return 0;
            p[0] = msg->u.request_vote_resp.vote_granted ? 1 : 0;
            return WIRE_OFF_PAYLOAD + 1;

        case RAFT_MSG_APPEND_ENTRIES: {
            const uint8_t nc = msg->u.append_entries.entry_count;
            /* header (3 × uint64) + count (1) + entries (nc × 73) */
            size_t payload_needed = 25 + (size_t)nc * (8 + 8 + 64 + 1);
            if (remaining < payload_needed) return 0;
            write_u64_le(p,      msg->u.append_entries.prev_log_index);
            write_u64_le(p + 8,  msg->u.append_entries.prev_log_term);
            write_u64_le(p + 16, msg->u.append_entries.leader_commit);
            p[24] = nc;
            uint8_t *ep = p + 25;
            for (int i = 0; i < nc; i++) {
                const RaftLogEntry *e = &msg->u.append_entries.entries[i];
                write_u64_le(ep,     e->index);
                write_u64_le(ep + 8, e->term);
                ep[16] = e->data_len;
                memcpy(ep + 17, e->data, e->data_len <= 64 ? e->data_len : 64);
                ep += (8 + 8 + 1 + 64);
            }
            return (size_t)(ep - buf);
        }

        case RAFT_MSG_APPEND_ENTRIES_RESP:
            if (remaining < 9) return 0;
            p[0] = msg->u.append_entries_resp.success ? 1 : 0;
            write_u64_le(p + 1, msg->u.append_entries_resp.match_index);
            return WIRE_OFF_PAYLOAD + 9;

        default:
            return 0;
    }
}

/*
 * Deserialise a wire buffer into a RaftMsg.
 * Returns true on success.
 */
static bool raft_msg_deserialise(const uint8_t *buf, size_t len, RaftMsg *msg) {
    if (len < WIRE_HEADER_SIZE) return false;

    memset(msg, 0, sizeof(RaftMsg));
    msg->type      = (RaftMsgType)buf[WIRE_OFF_TYPE];
    msg->from_node = buf[WIRE_OFF_FROM];
    msg->term      = read_u64_le(buf + WIRE_OFF_TERM);

    const uint8_t *p      = buf + WIRE_OFF_PAYLOAD;
    size_t         avail  = len - WIRE_OFF_PAYLOAD;

    switch (msg->type) {
        case RAFT_MSG_REQUEST_VOTE:
            if (avail < 16) return false;
            msg->u.request_vote.last_log_index = read_u64_le(p);
            msg->u.request_vote.last_log_term  = read_u64_le(p + 8);
            return true;

        case RAFT_MSG_REQUEST_VOTE_RESP:
            if (avail < 1) return false;
            msg->u.request_vote_resp.vote_granted = (p[0] != 0);
            return true;

        case RAFT_MSG_APPEND_ENTRIES: {
            if (avail < 25) return false;
            msg->u.append_entries.prev_log_index = read_u64_le(p);
            msg->u.append_entries.prev_log_term  = read_u64_le(p + 8);
            msg->u.append_entries.leader_commit  = read_u64_le(p + 16);
            uint8_t nc = p[24];
            if (nc > 8) nc = 8; /* clamp to RaftMsg limit */
            msg->u.append_entries.entry_count = nc;
            const uint8_t *ep = p + 25;
            for (int i = 0; i < nc; i++) {
                if ((size_t)(ep - buf) + 17 > len) return false;
                RaftLogEntry *e = &msg->u.append_entries.entries[i];
                e->index    = read_u64_le(ep);
                e->term     = read_u64_le(ep + 8);
                e->data_len = ep[16];
                if (e->data_len > 64) e->data_len = 64;
                if ((size_t)(ep - buf) + 17 + e->data_len > len) return false;
                memcpy(e->data, ep + 17, e->data_len);
                ep += (8 + 8 + 1 + 64);
            }
            return true;
        }

        case RAFT_MSG_APPEND_ENTRIES_RESP:
            if (avail < 9) return false;
            msg->u.append_entries_resp.success     = (p[0] != 0);
            msg->u.append_entries_resp.match_index = read_u64_le(p + 1);
            return true;

        default:
            return false;
    }
}

/* ── Global Raft node ────────────────────────────────────────────────── */

static RaftNode g_raft;
static bool     g_initialized = false;

/* ── GPU task queue (committed entries land here) ────────────────────── */

#define GPU_QUEUE_MAX 64

typedef struct {
    uint64_t seq;
    uint8_t  data[64];
    uint8_t  len;
} GPUTask;

static GPUTask  g_gpu_queue[GPU_QUEUE_MAX];
static int      g_gpu_queue_head  = 0;
static int      g_gpu_queue_count = 0;

/* ── Apply callback (called on Raft commit) ───────────────────────────── */

static void on_commit(void *ud, const RaftLogEntry *e) {
    (void)ud;
    if (g_gpu_queue_count >= GPU_QUEUE_MAX) return; /* queue full — drop */
    GPUTask *t = &g_gpu_queue[(g_gpu_queue_head + g_gpu_queue_count) % GPU_QUEUE_MAX];
    t->seq = e->index;
    memcpy(t->data, e->data, e->data_len <= 64 ? e->data_len : 64);
    t->len = e->data_len;
    g_gpu_queue_count++;
    printf("[mesh_raft] committed GPU task seq=%llu len=%u\n",
           (unsigned long long)e->index, (unsigned)e->data_len);
}

/* ── Raft send callback: serialise and publish via MsgBus topic ──────── */

static void raft_send(void *ud, uint8_t to_node, const RaftMsg *msg) {
    (void)ud;

    /* Build topic: "raft.<to_node>.<msg_type>" */
    char topic[64];
    snprintf(topic, sizeof(topic), "raft.%u.%u", (unsigned)to_node, (unsigned)msg->type);

    /* Serialise */
    uint8_t wire[WIRE_MAX_SIZE];
    size_t  wire_len = raft_msg_serialise(msg, wire, sizeof(wire));
    if (wire_len == 0) {
        printf("[mesh_raft] serialise failed for type=%d to=%u\n",
               (int)msg->type, (unsigned)to_node);
        return;
    }

    /* Dispatch via pluggable transport */
    if (g_send_fn) {
        g_send_fn(g_send_ud, topic, wire, wire_len);
    } else {
        /* Fallback: log the send (useful in unit tests / early bringup) */
        printf("[mesh_raft] send topic=%s wire_len=%zu type=%d term=%llu to=%u\n",
               topic, wire_len, (int)msg->type,
               (unsigned long long)msg->term, (unsigned)to_node);
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void mesh_raft_init(uint8_t self_id, const char *self_name) {
    raft_init(&g_raft, self_id, self_name, raft_send, on_commit, NULL);
    /* Default two-node cluster: rocky(0) + natasha(1) */
    if (self_id != 0) raft_add_peer(&g_raft, 0, "rocky");
    if (self_id != 1) raft_add_peer(&g_raft, 1, "natasha");
    raft_reset_election_timer(&g_raft, 0);
    g_initialized = true;
    printf("[mesh_raft] initialized node %u (%s), cluster_size=%d\n",
           (unsigned)self_id, self_name ? self_name : "?",
           g_raft.peer_count + 1);
}

void mesh_raft_tick(uint64_t now_ms) {
    if (!g_initialized) return;
    raft_tick(&g_raft, now_ms);
}

/*
 * Receive an incoming wire-format Raft message from the network layer.
 * The caller (mesh_agent dispatch loop) should call this when a message
 * arrives on any topic matching "raft.*".
 */
void mesh_raft_on_wire(const uint8_t *buf, size_t len, uint64_t now_ms) {
    if (!g_initialized) return;
    RaftMsg msg;
    if (!raft_msg_deserialise(buf, len, &msg)) {
        printf("[mesh_raft] deserialise failed (len=%zu)\n", len);
        return;
    }
    raft_handle_msg(&g_raft, &msg, now_ms);
}

/*
 * Accept an already-deserialised RaftMsg (e.g. from an in-process test).
 */
void mesh_raft_on_msg(const RaftMsg *msg, uint64_t now_ms) {
    if (!g_initialized) return;
    raft_handle_msg(&g_raft, msg, now_ms);
}

bool mesh_raft_propose_gpu_task(const uint8_t *data, uint8_t len) {
    if (!g_initialized) return false;
    return raft_propose(&g_raft, data, len);
}

bool mesh_raft_is_leader(void) {
    return g_initialized && raft_is_leader(&g_raft);
}

int8_t mesh_raft_leader_id(void) {
    if (!g_initialized) return -1;
    return raft_leader(&g_raft);
}

/* Dequeue a committed GPU task.  Returns true if a task was available. */
bool mesh_raft_dequeue_gpu_task(uint8_t *data_out, uint8_t *len_out) {
    if (g_gpu_queue_count == 0) return false;
    GPUTask *t = &g_gpu_queue[g_gpu_queue_head];
    if (data_out) memcpy(data_out, t->data, t->len);
    if (len_out)  *len_out = t->len;
    g_gpu_queue_head = (g_gpu_queue_head + 1) % GPU_QUEUE_MAX;
    g_gpu_queue_count--;
    return true;
}

/*
 * Convenience: decode a wire buffer received from the SquirrelBus and
 * return the deserialised RaftMsg to the caller.
 * Returns true on success; msg is populated.
 */
bool mesh_raft_decode(const uint8_t *buf, size_t len, RaftMsg *msg_out) {
    return raft_msg_deserialise(buf, len, msg_out);
}
