/*
 * raft.h — Raft consensus for agentOS multi-board mesh
 *
 * Implements Raft leader election and log replication for the agentOS mesh,
 * enabling automatic failover when Rocky (do-host1) goes offline and
 * consistent GPU task queue state across sparky and do-host1.
 *
 * Scope: leader election + log replication for GPU scheduler queue state.
 *        Non-GPU queue state remains eventually consistent (no Raft).
 *
 * Transport: messages are exchanged via the RCC SquirrelBus
 *   (POST /api/squirrelbus/raft) or directly via TCP (port 8790).
 *   Agents call raft_tick() periodically from their heartbeat loop.
 *
 * Cluster: up to RAFT_MAX_NODES members.  Default: rocky + natasha (sparky).
 *
 * Reference: Ongaro & Ousterhout "In Search of an Understandable Consensus
 *            Algorithm" (2014). Section numbers in comments refer to the paper.
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#pragma once
#ifndef RAFT_H
#define RAFT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Configuration ────────────────────────────────────────────────────── */

#define RAFT_MAX_NODES       5
#define RAFT_MAX_LOG         256   /* in-memory log entries */
#define RAFT_MAX_NODE_NAME   32
#define RAFT_HEARTBEAT_MS    150   /* leader heartbeat interval */
#define RAFT_ELECTION_MIN_MS 300   /* min election timeout */
#define RAFT_ELECTION_MAX_MS 600   /* max election timeout */

/* ── Node states (§5) ────────────────────────────────────────────────── */

typedef enum {
    RAFT_STATE_FOLLOWER  = 0,
    RAFT_STATE_CANDIDATE = 1,
    RAFT_STATE_LEADER    = 2,
} RaftState;

/* ── Log entry ────────────────────────────────────────────────────────── */

/* GPU task queue entry replicated via Raft */
typedef struct {
    uint64_t    index;      /* log index (1-based) */
    uint64_t    term;       /* term when entry was received */
    uint8_t     data[64];   /* opaque payload (e.g. gpu_tensor_desc_t summary) */
    uint8_t     data_len;
} RaftLogEntry;

/* ── RPC message types ────────────────────────────────────────────────── */

typedef enum {
    RAFT_MSG_REQUEST_VOTE     = 1,
    RAFT_MSG_REQUEST_VOTE_RESP= 2,
    RAFT_MSG_APPEND_ENTRIES   = 3,
    RAFT_MSG_APPEND_ENTRIES_RESP=4,
    RAFT_MSG_INSTALL_SNAPSHOT = 5,
} RaftMsgType;

typedef struct {
    RaftMsgType type;
    uint64_t    term;
    uint8_t     from_node;
    union {
        /* RequestVote (§5.2) */
        struct {
            uint64_t last_log_index;
            uint64_t last_log_term;
        } request_vote;
        /* RequestVoteResponse */
        struct {
            bool vote_granted;
        } request_vote_resp;
        /* AppendEntries (§5.3) — also heartbeat when entry_count=0 */
        struct {
            uint64_t      prev_log_index;
            uint64_t      prev_log_term;
            uint64_t      leader_commit;
            uint8_t       entry_count;
            RaftLogEntry  entries[8]; /* max 8 entries per RPC */
        } append_entries;
        /* AppendEntriesResponse */
        struct {
            bool     success;
            uint64_t match_index;
        } append_entries_resp;
    } u;
} RaftMsg;

/* ── Peer ─────────────────────────────────────────────────────────────── */

typedef struct {
    char     name[RAFT_MAX_NODE_NAME];
    uint8_t  id;
    bool     active;
    uint64_t next_index;    /* leader only: next log index to send */
    uint64_t match_index;   /* leader only: highest replicated index */
    uint64_t last_contact;  /* ms timestamp of last RPC response */
} RaftPeer;

/* ── Send callback ────────────────────────────────────────────────────── */

/* Called by raft.c to send a message to peer.  Caller implements transport. */
typedef void (*RaftSendFn)(void *userdata, uint8_t to_node_id,
                            const RaftMsg *msg);

/* ── State machine apply callback ────────────────────────────────────── */

/* Called when a log entry is committed (applied to state machine).
 * For GPU queue: update the in-memory GPU task queue. */
typedef void (*RaftApplyFn)(void *userdata, const RaftLogEntry *entry);

/* ── Raft node state ──────────────────────────────────────────────────── */

typedef struct {
    /* Identity */
    uint8_t      self_id;
    char         self_name[RAFT_MAX_NODE_NAME];

    /* Persistent state (§5) */
    uint64_t     current_term;
    int8_t       voted_for;    /* -1 = no vote */
    RaftLogEntry log[RAFT_MAX_LOG];
    uint64_t     log_count;

    /* Volatile state */
    uint64_t     commit_index;
    uint64_t     last_applied;
    RaftState    state;
    int8_t       leader_id;    /* -1 if unknown */

    /* Election timer */
    uint64_t     election_deadline_ms;   /* ms timestamp */
    uint64_t     next_heartbeat_ms;      /* leader: next heartbeat time */

    /* Votes received (candidate state) */
    uint8_t      votes_received;

    /* Cluster peers */
    RaftPeer     peers[RAFT_MAX_NODES];
    uint8_t      peer_count;

    /* Callbacks */
    RaftSendFn   send;
    RaftApplyFn  apply;
    void        *userdata;
} RaftNode;

/* ── API ──────────────────────────────────────────────────────────────── */

/*
 * Initialise a Raft node.
 * self_id: this node's unique integer ID (0-based).
 * send:    transport callback for sending RPCs.
 * apply:   state machine apply callback (called on commit).
 */
void raft_init(RaftNode *r, uint8_t self_id, const char *self_name,
               RaftSendFn send, RaftApplyFn apply, void *userdata);

/*
 * Add a peer to the cluster.  Call before raft_init completes.
 */
void raft_add_peer(RaftNode *r, uint8_t id, const char *name);

/*
 * Drive the Raft state machine.  Call periodically (every ~10ms).
 * now_ms: current monotonic time in milliseconds.
 */
void raft_tick(RaftNode *r, uint64_t now_ms);

/*
 * Handle an incoming Raft message.
 */
void raft_handle_msg(RaftNode *r, const RaftMsg *msg, uint64_t now_ms);

/*
 * Propose a new log entry (leader only).
 * Returns true if accepted (node is leader), false otherwise.
 */
bool raft_propose(RaftNode *r, const uint8_t *data, uint8_t data_len);

/*
 * Convenience: seed the election timeout with a pseudo-random value
 * derived from node_id and current time.
 */
void raft_reset_election_timer(RaftNode *r, uint64_t now_ms);

/*
 * Return current state/leader info.
 */
RaftState raft_state(const RaftNode *r);
int8_t    raft_leader(const RaftNode *r);
bool      raft_is_leader(const RaftNode *r);
uint64_t  raft_commit_index(const RaftNode *r);

#endif /* RAFT_H */
