/*
 * raft.c — Raft consensus implementation for agentOS mesh
 *
 * Implements §5 of the Raft paper (Ongaro & Ousterhout, 2014).
 * No dynamic allocation. Thread-safe if caller serialises raft_tick/handle_msg.
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#include "raft.h"
#include <string.h>

/* ── Pseudo-random election timeout (LCG seeded by node_id+time) ─────── */

static uint64_t lcg_state = 0;
static uint32_t lcg_rand(void) {
    lcg_state = lcg_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(lcg_state >> 33);
}

void raft_reset_election_timer(RaftNode *r, uint64_t now_ms) {
    lcg_state ^= (uint64_t)r->self_id * 0xdeadbeef ^ now_ms;
    uint32_t range = RAFT_ELECTION_MAX_MS - RAFT_ELECTION_MIN_MS;
    r->election_deadline_ms = now_ms + RAFT_ELECTION_MIN_MS
                              + (lcg_rand() % range);
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static uint8_t quorum(const RaftNode *r) {
    return (uint8_t)(r->peer_count / 2 + 1);
}

static RaftPeer *find_peer(RaftNode *r, uint8_t id) {
    for (int i = 0; i < r->peer_count; i++)
        if (r->peers[i].id == id) return &r->peers[i];
    return NULL;
}

static void become_follower(RaftNode *r, uint64_t term, uint64_t now_ms) {
    r->state        = RAFT_STATE_FOLLOWER;
    r->current_term = term;
    r->voted_for    = -1;
    r->leader_id    = -1;
    raft_reset_election_timer(r, now_ms);
}

static void become_candidate(RaftNode *r, uint64_t now_ms) {
    r->state          = RAFT_STATE_CANDIDATE;
    r->current_term++;
    r->voted_for      = (int8_t)r->self_id;
    r->votes_received = 1; /* vote for self */
    raft_reset_election_timer(r, now_ms);

    /* Send RequestVote to all peers */
    uint64_t last_idx  = r->log_count;
    uint64_t last_term = last_idx ? r->log[last_idx-1].term : 0;
    for (int i = 0; i < r->peer_count; i++) {
        if (!r->peers[i].active) continue;
        RaftMsg m = {0};
        m.type = RAFT_MSG_REQUEST_VOTE;
        m.term = r->current_term;
        m.from_node = r->self_id;
        m.u.request_vote.last_log_index = last_idx;
        m.u.request_vote.last_log_term  = last_term;
        r->send(r->userdata, r->peers[i].id, &m);
    }
}

static void become_leader(RaftNode *r, uint64_t now_ms) {
    r->state     = RAFT_STATE_LEADER;
    r->leader_id = (int8_t)r->self_id;
    /* Initialise next_index and match_index for each peer (§5.3) */
    for (int i = 0; i < r->peer_count; i++) {
        r->peers[i].next_index  = r->log_count + 1;
        r->peers[i].match_index = 0;
    }
    r->next_heartbeat_ms = now_ms;
    /* Send immediate heartbeat */
    r->next_heartbeat_ms = now_ms; /* triggers send on next tick */
}

/* ── Apply committed entries ─────────────────────────────────────────── */

static void apply_committed(RaftNode *r) {
    while (r->last_applied < r->commit_index &&
           r->last_applied < r->log_count) {
        r->last_applied++;
        if (r->apply)
            r->apply(r->userdata, &r->log[r->last_applied - 1]);
    }
}

/* ── Leader: send AppendEntries to a peer ─────────────────────────────── */

static void send_append_entries(RaftNode *r, RaftPeer *peer, uint64_t now_ms) {
    (void)now_ms;
    RaftMsg m = {0};
    m.type      = RAFT_MSG_APPEND_ENTRIES;
    m.term      = r->current_term;
    m.from_node = r->self_id;

    uint64_t prev_idx  = peer->next_index - 1;
    uint64_t prev_term = (prev_idx > 0 && prev_idx <= r->log_count)
                         ? r->log[prev_idx-1].term : 0;

    m.u.append_entries.prev_log_index = prev_idx;
    m.u.append_entries.prev_log_term  = prev_term;
    m.u.append_entries.leader_commit  = r->commit_index;

    /* Pack up to 8 entries starting at next_index */
    uint8_t n = 0;
    uint64_t idx = peer->next_index;
    while (n < 8 && idx <= r->log_count) {
        m.u.append_entries.entries[n++] = r->log[idx-1];
        idx++;
    }
    m.u.append_entries.entry_count = n;

    r->send(r->userdata, peer->id, &m);
}

/* ── Handle RequestVote ───────────────────────────────────────────────── */

static void handle_request_vote(RaftNode *r, const RaftMsg *msg, uint64_t now_ms) {
    bool grant = false;
    uint64_t my_last_idx  = r->log_count;
    uint64_t my_last_term = my_last_idx ? r->log[my_last_idx-1].term : 0;

    if (msg->term >= r->current_term) {
        if (msg->term > r->current_term)
            become_follower(r, msg->term, now_ms);

        bool log_ok = (msg->u.request_vote.last_log_term > my_last_term) ||
                      (msg->u.request_vote.last_log_term == my_last_term &&
                       msg->u.request_vote.last_log_index >= my_last_idx);

        if ((r->voted_for < 0 || r->voted_for == (int8_t)msg->from_node) && log_ok) {
            r->voted_for = (int8_t)msg->from_node;
            grant = true;
            raft_reset_election_timer(r, now_ms); /* reset — heard from candidate */
        }
    }

    RaftMsg resp = {0};
    resp.type = RAFT_MSG_REQUEST_VOTE_RESP;
    resp.term = r->current_term;
    resp.from_node = r->self_id;
    resp.u.request_vote_resp.vote_granted = grant;
    r->send(r->userdata, msg->from_node, &resp);
}

/* ── Handle RequestVoteResponse ─────────────────────────────────────── */

static void handle_request_vote_resp(RaftNode *r, const RaftMsg *msg,
                                      uint64_t now_ms) {
    if (r->state != RAFT_STATE_CANDIDATE) return;
    if (msg->term > r->current_term) { become_follower(r, msg->term, now_ms); return; }
    if (msg->term < r->current_term) return;

    if (msg->u.request_vote_resp.vote_granted) {
        r->votes_received++;
        if (r->votes_received >= quorum(r))
            become_leader(r, now_ms);
    }
}

/* ── Handle AppendEntries ─────────────────────────────────────────────── */

typedef struct {
    uint64_t prev_log_index;
    uint64_t prev_log_term;
    uint64_t leader_commit;
    uint8_t  entry_count;
    RaftLogEntry entries[8];
} _AEPayload;

static void handle_append_entries(RaftNode *r, const RaftMsg *msg, uint64_t now_ms) {
    bool success = false;
    uint64_t match_index = 0;
    const _AEPayload *ae = (const _AEPayload *)&msg->u.append_entries;

    if (msg->term < r->current_term) goto respond;

    /* Valid leader — reset election timer, update term */
    if (msg->term > r->current_term)
        become_follower(r, msg->term, now_ms);
    else {
        r->state     = RAFT_STATE_FOLLOWER;
        r->leader_id = (int8_t)msg->from_node;
        raft_reset_election_timer(r, now_ms);
    }
    r->leader_id = (int8_t)msg->from_node;

    /* Log consistency check (§5.3) */
    if (ae->prev_log_index > 0) {
        if (ae->prev_log_index > r->log_count) goto respond;
        if (r->log[ae->prev_log_index-1].term != ae->prev_log_term) {
            /* Conflict: truncate log */
            r->log_count = ae->prev_log_index - 1;
            goto respond;
        }
    }

    /* Append new entries */
    uint64_t idx = ae->prev_log_index;
    for (int i = 0; i < ae->entry_count; i++) {
        idx++;
        if (idx <= r->log_count &&
            r->log[idx-1].term != ae->entries[i].term) {
            /* Conflicting entry: truncate */
            r->log_count = idx - 1;
        }
        if (idx > r->log_count) {
            if (r->log_count < RAFT_MAX_LOG) {
                r->log[r->log_count++] = ae->entries[i];
            }
        }
    }

    /* Update commit index */
    if (ae->leader_commit > r->commit_index) {
        r->commit_index = ae->leader_commit < r->log_count
                          ? ae->leader_commit : r->log_count;
        apply_committed(r);
    }

    success     = true;
    match_index = r->log_count;

respond: {
        RaftMsg resp = {0};
        resp.type = RAFT_MSG_APPEND_ENTRIES_RESP;
        resp.term = r->current_term;
        resp.from_node = r->self_id;
        resp.u.append_entries_resp.success     = success;
        resp.u.append_entries_resp.match_index = match_index;
        r->send(r->userdata, msg->from_node, &resp);
    }
}

/* ── Handle AppendEntriesResponse ────────────────────────────────────── */

static void handle_append_entries_resp(RaftNode *r, const RaftMsg *msg,
                                        uint64_t now_ms) {
    if (r->state != RAFT_STATE_LEADER) return;
    if (msg->term > r->current_term) { become_follower(r, msg->term, now_ms); return; }

    RaftPeer *peer = find_peer(r, msg->from_node);
    if (!peer) return;

    peer->last_contact = now_ms;

    if (msg->u.append_entries_resp.success) {
        peer->match_index = msg->u.append_entries_resp.match_index;
        peer->next_index  = peer->match_index + 1;

        /* Check if we can advance commit_index (§5.3, majority rule) */
        for (uint64_t n = r->log_count; n > r->commit_index; n--) {
            if (r->log[n-1].term != r->current_term) continue;
            uint8_t count = 1; /* self */
            for (int i = 0; i < r->peer_count; i++)
                if (r->peers[i].match_index >= n) count++;
            if (count >= quorum(r)) {
                r->commit_index = n;
                apply_committed(r);
                break;
            }
        }
    } else {
        /* Decrement next_index and retry */
        if (peer->next_index > 1) peer->next_index--;
        send_append_entries(r, peer, now_ms);
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

void raft_init(RaftNode *r, uint8_t self_id, const char *self_name,
               RaftSendFn send, RaftApplyFn apply, void *userdata) {
    memset(r, 0, sizeof(RaftNode));
    r->self_id  = self_id;
    r->voted_for= -1;
    r->leader_id= -1;
    r->state    = RAFT_STATE_FOLLOWER;
    r->send     = send;
    r->apply    = apply;
    r->userdata = userdata;
    if (self_name)
        strncpy(r->self_name, self_name, RAFT_MAX_NODE_NAME-1);
    lcg_state = (uint64_t)self_id * 0xcafebabe;
    r->election_deadline_ms = RAFT_ELECTION_MIN_MS;
}

void raft_add_peer(RaftNode *r, uint8_t id, const char *name) {
    if (r->peer_count >= RAFT_MAX_NODES) return;
    RaftPeer *p = &r->peers[r->peer_count++];
    memset(p, 0, sizeof(RaftPeer));
    p->id     = id;
    p->active = true;
    p->next_index = 1;
    if (name) strncpy(p->name, name, RAFT_MAX_NODE_NAME-1);
}

void raft_tick(RaftNode *r, uint64_t now_ms) {
    if (r->state == RAFT_STATE_LEADER) {
        /* Send heartbeat if due */
        if (now_ms >= r->next_heartbeat_ms) {
            r->next_heartbeat_ms = now_ms + RAFT_HEARTBEAT_MS;
            for (int i = 0; i < r->peer_count; i++) {
                if (r->peers[i].active)
                    send_append_entries(r, &r->peers[i], now_ms);
            }
        }
    } else {
        /* Check election timeout (§5.2) */
        if (now_ms >= r->election_deadline_ms)
            become_candidate(r, now_ms);
    }
}

void raft_handle_msg(RaftNode *r, const RaftMsg *msg, uint64_t now_ms) {
    if (!r || !msg) return;
    switch (msg->type) {
        case RAFT_MSG_REQUEST_VOTE:
            handle_request_vote(r, msg, now_ms); break;
        case RAFT_MSG_REQUEST_VOTE_RESP:
            handle_request_vote_resp(r, msg, now_ms); break;
        case RAFT_MSG_APPEND_ENTRIES:
            handle_append_entries(r, msg, now_ms); break;
        case RAFT_MSG_APPEND_ENTRIES_RESP:
            handle_append_entries_resp(r, msg, now_ms); break;
        default: break;
    }
}

bool raft_propose(RaftNode *r, const uint8_t *data, uint8_t data_len) {
    if (!raft_is_leader(r)) return false;
    if (r->log_count >= RAFT_MAX_LOG) return false;
    if (data_len > 64) data_len = 64;

    RaftLogEntry *e = &r->log[r->log_count++];
    e->index    = r->log_count;
    e->term     = r->current_term;
    e->data_len = data_len;
    if (data && data_len)
        memcpy(e->data, data, data_len);

    /* Immediately send to all peers */
    for (int i = 0; i < r->peer_count; i++) {
        if (r->peers[i].active)
            send_append_entries(r, &r->peers[i], 0);
    }
    return true;
}

RaftState raft_state(const RaftNode *r)        { return r->state; }
int8_t    raft_leader(const RaftNode *r)        { return r->leader_id; }
bool      raft_is_leader(const RaftNode *r)     { return r->state == RAFT_STATE_LEADER; }
uint64_t  raft_commit_index(const RaftNode *r)  { return r->commit_index; }
