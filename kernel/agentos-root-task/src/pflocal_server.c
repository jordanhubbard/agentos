/*
 * pflocal_server.c — AF_UNIX (local) socket emulation for agentOS
 *
 * HURD-equivalent: pflocal translator
 * Priority: 155 (passive)
 *
 * Provides POSIX-style Unix domain sockets over shmem ring buffers.
 * Supports up to PFLOCAL_MAX_SOCKS sockets with 4KB I/O slots each.
 *
 * Channel assignments:
 *   id=0: receives PPC from controller (CH_PFLOCAL_SERVER = 26)
 *
 * Shared memory layout (pflocal_shmem, 64KB total):
 *   Slot N (N=0..15):
 *     [0x000..0x00F]  ring_hdr_t  (head=4, tail=4, cap=4, magic=4)
 *     [0x010..0xFFF]  4080 bytes ring data
 *
 * Opcodes (all in agentos.h):
 *   OP_PFLOCAL_SOCKET  0xD0  → ok + sock_id + slot_offset
 *   OP_PFLOCAL_BIND    0xD1  MR1=sock_id  path in shmem slot 0..7 bytes
 *   OP_PFLOCAL_LISTEN  0xD2  MR1=sock_id
 *   OP_PFLOCAL_CONNECT 0xD3  MR1=sock_id  path in shmem slot 0..7 bytes
 *   OP_PFLOCAL_ACCEPT  0xD4  MR1=sock_id → ok + new_sock_id + peer_slot
 *   OP_PFLOCAL_SEND    0xD5  MR1=sock_id MR2=shmem_offset MR3=len → ok+sent
 *   OP_PFLOCAL_RECV    0xD6  MR1=sock_id MR2=shmem_offset MR3=max → ok+recv
 *   OP_PFLOCAL_CLOSE   0xD7  MR1=sock_id
 *   OP_PFLOCAL_STATUS  0xD8  MR1=sock_id → ok+state+peer_sock_id
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "agentos.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ── Configuration ─────────────────────────────────────────────────────── */
#define PFLOCAL_MAX_SOCKS  16u
#define PFLOCAL_SLOT_SIZE  0x1000u   /* 4KB per socket slot */
#define RING_HDR_SIZE      16u
#define RING_DATA_SIZE     (PFLOCAL_SLOT_SIZE - RING_HDR_SIZE)  /* 4080 bytes */
#define RING_MAGIC         0x5246494eu  /* "NIFR" */
#define PFLOCAL_PATH_MAX   64u

/* Socket states */
#define SOCK_FREE      0u
#define SOCK_CREATED   1u
#define SOCK_BOUND     2u
#define SOCK_LISTENING 3u
#define SOCK_CONNECTED 4u
#define SOCK_ACCEPTED  5u

/* ── Shared memory ─────────────────────────────────────────────────────── */
uintptr_t pflocal_shmem_vaddr;   /* set by Microkit linker */

/* ── Ring buffer header (at start of each 4KB slot) ───────────────────── */
typedef struct {
    uint32_t head;
    uint32_t tail;
    uint32_t cap;
    uint32_t magic;
} ring_hdr_t;

/* ── Socket entry ─────────────────────────────────────────────────────── */
typedef struct {
    uint8_t  state;
    uint8_t  slot_id;          /* which 4KB slot owns the ring buffer */
    uint8_t  _pad[2];
    uint32_t peer_sock_id;     /* linked socket (after ACCEPT) */
    uint32_t backlog_sock_id;  /* pending accept queue (single entry) */
    char     path[PFLOCAL_PATH_MAX];
} pflocal_sock_t;

static pflocal_sock_t socks[PFLOCAL_MAX_SOCKS];
static uint32_t       next_sock_id = 1;

/* ── Helpers ──────────────────────────────────────────────────────────── */
static inline ring_hdr_t *slot_hdr(uint8_t slot_id)
{
    return (ring_hdr_t *)(pflocal_shmem_vaddr + (uintptr_t)slot_id * PFLOCAL_SLOT_SIZE);
}

static inline uint8_t *slot_data(uint8_t slot_id)
{
    return (uint8_t *)(pflocal_shmem_vaddr + (uintptr_t)slot_id * PFLOCAL_SLOT_SIZE
                       + RING_HDR_SIZE);
}

static void ring_init(uint8_t slot_id)
{
    ring_hdr_t *h = slot_hdr(slot_id);
    h->head  = 0;
    h->tail  = 0;
    h->cap   = RING_DATA_SIZE;
    h->magic = RING_MAGIC;
}

static uint32_t ring_write(uint8_t slot_id, const uint8_t *src, uint32_t len)
{
    ring_hdr_t *h    = slot_hdr(slot_id);
    uint8_t    *data = slot_data(slot_id);
    uint32_t    free_space;

    if (h->magic != RING_MAGIC) return 0;
    if (h->tail >= h->head)
        free_space = h->cap - (h->tail - h->head);
    else
        free_space = h->head - h->tail;
    if (free_space == 0) return 0;
    if (len > free_space) len = free_space;

    for (uint32_t i = 0; i < len; i++) {
        data[h->tail % h->cap] = src[i];
        h->tail++;
    }
    return len;
}

static uint32_t ring_read(uint8_t slot_id, uint8_t *dst, uint32_t max)
{
    ring_hdr_t *h    = slot_hdr(slot_id);
    uint8_t    *data = slot_data(slot_id);
    uint32_t    avail;

    if (h->magic != RING_MAGIC) return 0;
    avail = h->tail - h->head;
    if (avail == 0) return 0;
    if (max > avail) max = avail;

    for (uint32_t i = 0; i < max; i++) {
        dst[i] = data[h->head % h->cap];
        h->head++;
    }
    return max;
}

static pflocal_sock_t *find_sock(uint32_t sock_id)
{
    for (uint32_t i = 0; i < PFLOCAL_MAX_SOCKS; i++) {
        if (socks[i].state != SOCK_FREE && (i + 1u) == sock_id)
            return &socks[i];
    }
    return NULL;
}

static pflocal_sock_t *find_by_path(const char *path)
{
    for (uint32_t i = 0; i < PFLOCAL_MAX_SOCKS; i++) {
        if (socks[i].state >= SOCK_BOUND) {
            /* strncmp without libc */
            const char *a = socks[i].path, *b = path;
            bool match = true;
            for (uint32_t j = 0; j < PFLOCAL_PATH_MAX; j++) {
                if (a[j] != b[j]) { match = false; break; }
                if (a[j] == '\0') break;
            }
            if (match) return &socks[i];
        }
    }
    return NULL;
}

static uint8_t find_free_slot(void)
{
    for (uint8_t i = 0; i < PFLOCAL_MAX_SOCKS; i++) {
        ring_hdr_t *h = slot_hdr(i);
        if (h->magic != RING_MAGIC) return i;
    }
    return 0xFF;
}

/* ── IPC dispatch ─────────────────────────────────────────────────────── */
static microkit_msginfo handle_socket(void)
{
    /* find free sock entry */
    uint32_t idx = PFLOCAL_MAX_SOCKS;
    for (uint32_t i = 0; i < PFLOCAL_MAX_SOCKS; i++) {
        if (socks[i].state == SOCK_FREE) { idx = i; break; }
    }
    if (idx == PFLOCAL_MAX_SOCKS) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    uint8_t slot = find_free_slot();
    if (slot == 0xFF) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    ring_init(slot);
    socks[idx].state        = SOCK_CREATED;
    socks[idx].slot_id      = slot;
    socks[idx].peer_sock_id = 0;
    socks[idx].backlog_sock_id = 0;
    socks[idx].path[0]      = '\0';

    uint32_t sock_id = idx + 1u;
    uint32_t offset  = (uint32_t)(idx * PFLOCAL_SLOT_SIZE);

    microkit_mr_set(0, 1);
    microkit_mr_set(1, sock_id);
    microkit_mr_set(2, offset);
    return microkit_msginfo_new(0, 3);
}

static microkit_msginfo handle_bind(void)
{
    uint32_t sock_id = (uint32_t)microkit_mr_get(1);
    pflocal_sock_t *s = find_sock(sock_id);
    if (!s || s->state != SOCK_CREATED) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    /* path is in shmem slot for this socket, at slot_data offset 0 */
    const char *path = (const char *)slot_data(s->slot_id);
    uint32_t i = 0;
    for (; i < PFLOCAL_PATH_MAX - 1 && path[i] != '\0'; i++)
        s->path[i] = path[i];
    s->path[i] = '\0';
    s->state = SOCK_BOUND;

    microkit_mr_set(0, 1);
    return microkit_msginfo_new(0, 1);
}

static microkit_msginfo handle_listen(void)
{
    uint32_t sock_id = (uint32_t)microkit_mr_get(1);
    pflocal_sock_t *s = find_sock(sock_id);
    if (!s || s->state != SOCK_BOUND) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }
    s->state = SOCK_LISTENING;
    microkit_mr_set(0, 1);
    return microkit_msginfo_new(0, 1);
}

static microkit_msginfo handle_connect(void)
{
    uint32_t sock_id = (uint32_t)microkit_mr_get(1);
    pflocal_sock_t *client = find_sock(sock_id);
    if (!client || client->state != SOCK_CREATED) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    /* path is at slot_data offset 0 of client's ring slot */
    const char *path = (const char *)slot_data(client->slot_id);
    pflocal_sock_t *server = find_by_path(path);
    if (!server || server->state != SOCK_LISTENING) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    uint32_t client_id = (uint32_t)(client - socks) + 1u;
    server->backlog_sock_id = client_id;
    client->state = SOCK_CONNECTED;

    microkit_mr_set(0, 1);
    return microkit_msginfo_new(0, 1);
}

static microkit_msginfo handle_accept(void)
{
    uint32_t sock_id = (uint32_t)microkit_mr_get(1);
    pflocal_sock_t *server = find_sock(sock_id);
    if (!server || server->state != SOCK_LISTENING || server->backlog_sock_id == 0) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    uint32_t client_id = server->backlog_sock_id;
    server->backlog_sock_id = 0;

    /* create a new accepted socket */
    uint32_t idx = PFLOCAL_MAX_SOCKS;
    for (uint32_t i = 0; i < PFLOCAL_MAX_SOCKS; i++) {
        if (socks[i].state == SOCK_FREE) { idx = i; break; }
    }
    if (idx == PFLOCAL_MAX_SOCKS) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    uint8_t slot = find_free_slot();
    if (slot == 0xFF) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    ring_init(slot);
    socks[idx].state        = SOCK_ACCEPTED;
    socks[idx].slot_id      = slot;
    socks[idx].peer_sock_id = client_id;
    socks[idx].backlog_sock_id = 0;
    socks[idx].path[0]      = '\0';

    /* wire the client's peer to the new accepted socket */
    pflocal_sock_t *client = find_sock(client_id);
    if (client) client->peer_sock_id = idx + 1u;

    uint32_t new_id = idx + 1u;
    microkit_mr_set(0, 1);
    microkit_mr_set(1, new_id);
    microkit_mr_set(2, (uint32_t)(idx * PFLOCAL_SLOT_SIZE));
    return microkit_msginfo_new(0, 3);
}

static microkit_msginfo handle_send(void)
{
    uint32_t sock_id = (uint32_t)microkit_mr_get(1);
    uint32_t offset  = (uint32_t)microkit_mr_get(2);
    uint32_t len     = (uint32_t)microkit_mr_get(3);
    pflocal_sock_t *s = find_sock(sock_id);

    if (!s || (s->state != SOCK_CONNECTED && s->state != SOCK_ACCEPTED)) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    /* write into the peer's ring buffer */
    pflocal_sock_t *peer = find_sock(s->peer_sock_id);
    if (!peer) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    const uint8_t *src = (const uint8_t *)(pflocal_shmem_vaddr + offset);
    uint32_t sent = ring_write(peer->slot_id, src, len);

    microkit_mr_set(0, 1);
    microkit_mr_set(1, sent);
    return microkit_msginfo_new(0, 2);
}

static microkit_msginfo handle_recv(void)
{
    uint32_t sock_id = (uint32_t)microkit_mr_get(1);
    uint32_t offset  = (uint32_t)microkit_mr_get(2);
    uint32_t max     = (uint32_t)microkit_mr_get(3);
    pflocal_sock_t *s = find_sock(sock_id);

    if (!s || (s->state != SOCK_CONNECTED && s->state != SOCK_ACCEPTED)) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    uint8_t *dst  = (uint8_t *)(pflocal_shmem_vaddr + offset);
    uint32_t recv = ring_read(s->slot_id, dst, max);

    microkit_mr_set(0, 1);
    microkit_mr_set(1, recv);
    return microkit_msginfo_new(0, 2);
}

static microkit_msginfo handle_close(void)
{
    uint32_t sock_id = (uint32_t)microkit_mr_get(1);
    pflocal_sock_t *s = find_sock(sock_id);
    if (!s) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    /* notify peer of closure */
    if (s->peer_sock_id != 0) {
        pflocal_sock_t *peer = find_sock(s->peer_sock_id);
        if (peer) peer->peer_sock_id = 0;
    }

    /* invalidate ring slot */
    ring_hdr_t *h = slot_hdr(s->slot_id);
    h->magic = 0;

    s->state        = SOCK_FREE;
    s->peer_sock_id = 0;
    s->path[0]      = '\0';

    microkit_mr_set(0, 1);
    return microkit_msginfo_new(0, 1);
}

static microkit_msginfo handle_status(void)
{
    uint32_t sock_id = (uint32_t)microkit_mr_get(1);
    pflocal_sock_t *s = find_sock(sock_id);
    if (!s) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }
    microkit_mr_set(0, 1);
    microkit_mr_set(1, s->state);
    microkit_mr_set(2, s->peer_sock_id);
    return microkit_msginfo_new(0, 3);
}

/* ── Microkit entry points ────────────────────────────────────────────── */
void init(void)
{
    /* Zero all socket state */
    for (uint32_t i = 0; i < PFLOCAL_MAX_SOCKS; i++) {
        socks[i].state        = SOCK_FREE;
        socks[i].slot_id      = (uint8_t)i;
        socks[i].peer_sock_id = 0;
        socks[i].backlog_sock_id = 0;
        socks[i].path[0]      = '\0';
    }

    microkit_dbg_puts("[pflocal] AF_UNIX socket server ready (16 slots, 64KB shmem)\n");
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg)
{
    (void)ch;
    (void)msg;
    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {
    case OP_PFLOCAL_SOCKET:  return handle_socket();
    case OP_PFLOCAL_BIND:    return handle_bind();
    case OP_PFLOCAL_LISTEN:  return handle_listen();
    case OP_PFLOCAL_CONNECT: return handle_connect();
    case OP_PFLOCAL_ACCEPT:  return handle_accept();
    case OP_PFLOCAL_SEND:    return handle_send();
    case OP_PFLOCAL_RECV:    return handle_recv();
    case OP_PFLOCAL_CLOSE:   return handle_close();
    case OP_PFLOCAL_STATUS:  return handle_status();
    default:
        microkit_dbg_puts("[pflocal] unknown opcode\n");
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }
}

void notified(microkit_channel ch)
{
    (void)ch;
    /* no async notifications needed */
}
