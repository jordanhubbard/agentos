/*
 * mesh_raft.c — Raft integration for agentOS mesh_agent
 *
 * Wires raft.c into the mesh agent loop:
 *   - Sends/receives RaftMsg via RCC SquirrelBus (POST /api/squirrelbus/raft)
 *   - Calls raft_tick() every ~10ms from the agent heartbeat
 *   - On commit: updates the in-memory GPU task queue
 *   - Exposes mesh_raft_is_leader() for the GPU scheduler guard
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
static int      g_gpu_queue_head = 0;
static int      g_gpu_queue_count = 0;

/* ── Apply callback (called on Raft commit) ───────────────────────────── */

static void on_commit(void *ud, const RaftLogEntry *e) {
    (void)ud;
    if (g_gpu_queue_count >= GPU_QUEUE_MAX) return; /* queue full */
    GPUTask *t = &g_gpu_queue[(g_gpu_queue_head + g_gpu_queue_count) % GPU_QUEUE_MAX];
    t->seq = e->index;
    memcpy(t->data, e->data, e->data_len);
    t->len = e->data_len;
    g_gpu_queue_count++;
    printf("[mesh_raft] committed GPU task seq=%llu\n", (unsigned long long)e->index);
}

/* ── Send callback: POST to RCC SquirrelBus ────────────────────────────── */

static void raft_send(void *ud, uint8_t to_node, const RaftMsg *msg) {
    (void)ud;
    /* In production: POST to /api/squirrelbus/raft with JSON-encoded msg.
     * For seL4 builds: send via IPC to the remote node's mesh_agent PD.
     * This stub just logs the send. */
    printf("[mesh_raft] send type=%d term=%llu to=%d\n",
           (int)msg->type, (unsigned long long)msg->term, (int)to_node);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void mesh_raft_init(uint8_t self_id, const char *self_name) {
    raft_init(&g_raft, self_id, self_name, raft_send, on_commit, NULL);
    /* Cluster: rocky(0) + natasha(1) */
    if (self_id != 0) raft_add_peer(&g_raft, 0, "rocky");
    if (self_id != 1) raft_add_peer(&g_raft, 1, "natasha");
    raft_reset_election_timer(&g_raft, 0);
    g_initialized = true;
    printf("[mesh_raft] initialized node %d (%s), cluster_size=%d\n",
           self_id, self_name, g_raft.peer_count + 1);
}

void mesh_raft_tick(uint64_t now_ms) {
    if (!g_initialized) return;
    raft_tick(&g_raft, now_ms);
}

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

/* Dequeue a committed GPU task. Returns true if a task was available. */
bool mesh_raft_dequeue_gpu_task(uint8_t *data_out, uint8_t *len_out) {
    if (g_gpu_queue_count == 0) return false;
    GPUTask *t = &g_gpu_queue[g_gpu_queue_head];
    memcpy(data_out, t->data, t->len);
    *len_out = t->len;
    g_gpu_queue_head = (g_gpu_queue_head + 1) % GPU_QUEUE_MAX;
    g_gpu_queue_count--;
    return true;
}
