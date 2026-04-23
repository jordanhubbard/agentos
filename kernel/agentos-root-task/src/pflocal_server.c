/*
 * pflocal_server.c — AF_UNIX (local) socket emulation for agentOS
 *
 * HURD-equivalent: pflocal translator
 * Priority: 155 (passive)
 *
 * Provides POSIX-style Unix domain sockets over shmem ring buffers.
 * Supports up to PFLOCAL_MAX_SOCKS sockets with 4KB I/O slots each.
 *
 * Socket lifecycle:
 *   SOCKET → BOUND → LISTENING → ACCEPTING
 *   SOCKET → CONNECTING → CONNECTED
 *   Any state → CLOSED
 *
 * Channel assignments:
 *   id=0: receives PPC from controller (CH_PFLOCAL_SERVER = 26)
 *
 * Shared memory layout (pflocal_shmem, 64KB total):
 *   Slot N (N=0..15):
 *     [0x000..0x00F]  ring_header_t  (magic=4, head=2, tail=2, capacity=2, pad=6)
 *     [0x010..0xFFF]  ring data
 *   pflocal_shmem[0..63] — path staging area (null-terminated string)
 *
 * Opcodes (all in agentos.h):
 *   OP_PFLOCAL_SOCKET  0xD0  → ok + sock_id + slot_offset
 *   OP_PFLOCAL_BIND    0xD1  MR1=sock_id  path in pflocal_shmem[0]
 *   OP_PFLOCAL_LISTEN  0xD2  MR1=sock_id
 *   OP_PFLOCAL_CONNECT 0xD3  MR1=sock_id  path in pflocal_shmem[0]
 *   OP_PFLOCAL_ACCEPT  0xD4  MR1=sock_id → ok + new_sock_id + peer_slot
 *   OP_PFLOCAL_SEND    0xD5  MR1=sock_id MR2=shmem_offset MR3=len → ok+sent
 *   OP_PFLOCAL_RECV    0xD6  MR1=sock_id MR2=shmem_offset MR3=max → ok+recv
 *   OP_PFLOCAL_CLOSE   0xD7  MR1=sock_id
 *   OP_PFLOCAL_STATUS  0xD8  MR1=sock_id → ok+state+peer_sock_id
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"

/* ── Shared memory ─────────────────────────────────────────────────────────── */
uintptr_t pflocal_shmem_vaddr;
#define PFLOCAL_SHMEM_SIZE   0x10000u  /* 64KB = 16 slots × 4KB */
#define PFLOCAL_SLOT_SIZE    0x1000u   /* 4KB per slot */
#define PFLOCAL_MAX_SOCKS    16u
#define PFLOCAL_PATH_MAX     64u

/* Ring header at start of each slot */
typedef struct __attribute__((packed)) {
    uint32_t magic;    /* 0xAF4E4958 = "UNIX" */
    uint16_t head;     /* write head (producer) */
    uint16_t tail;     /* read tail (consumer) */
    uint16_t capacity; /* usable bytes = PFLOCAL_SLOT_SIZE - sizeof(ring_header_t) */
    uint16_t _pad;
    uint32_t _reserved;
} ring_header_t;

#define RING_MAGIC      0xAF4E4958u
#define RING_DATA_SIZE  (PFLOCAL_SLOT_SIZE - (uint32_t)sizeof(ring_header_t))

/* Socket states */
#define SOCK_FREE        0u
#define SOCK_CREATED     1u
#define SOCK_BOUND       2u
#define SOCK_LISTENING   3u
#define SOCK_CONNECTING  4u
#define SOCK_CONNECTED   5u
#define SOCK_ACCEPTED    5u   /* alias: accepted sockets also use CONNECTED state */
#define SOCK_CLOSED      6u

typedef struct {
    uint8_t  state;
    uint8_t  slot_id;      /* which shmem slot this socket owns */
    uint8_t  peer_sock_id; /* connected peer socket ID (0xFF = none) */
    uint8_t  _pad;
    char     path[PFLOCAL_PATH_MAX]; /* bind path (empty if not bound) */
} pflocal_sock_t;

static pflocal_sock_t socks[PFLOCAL_MAX_SOCKS];

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static ring_header_t *slot_ring(uint8_t slot_id)
{
    return (ring_header_t *)(pflocal_shmem_vaddr +
                             (uintptr_t)slot_id * PFLOCAL_SLOT_SIZE);
}

static uint8_t *slot_data(uint8_t slot_id)
{
    return (uint8_t *)(pflocal_shmem_vaddr +
                       (uintptr_t)slot_id * PFLOCAL_SLOT_SIZE +
                       sizeof(ring_header_t));
}

static void ring_init(uint8_t slot_id)
{
    ring_header_t *r = slot_ring(slot_id);
    r->head     = 0;
    r->tail     = 0;
    r->capacity = (uint16_t)RING_DATA_SIZE;
    r->_pad     = 0;
    r->_reserved = 0;
    r->magic    = RING_MAGIC;
}

/* Simple string equality — no libc */
static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0');
}

/* Copy at most n-1 bytes from src to dst, always null-terminate */
static void str_ncopy(char *dst, const char *src, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n - 1u && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/*
 * find_bound_sock — scan for a SOCK_LISTENING socket with matching path.
 * Returns sock_id [0..PFLOCAL_MAX_SOCKS-1] or -1 if not found.
 */
static int find_bound_sock(const char *path)
{
    for (uint32_t i = 0; i < PFLOCAL_MAX_SOCKS; i++) {
        if (socks[i].state == SOCK_LISTENING && str_eq(socks[i].path, path))
            return (int)i;
    }
    return -1;
}

/* ── Microkit entry points ─────────────────────────────────────────────────── */

void init(void)
{
    for (uint32_t i = 0; i < PFLOCAL_MAX_SOCKS; i++) {
        socks[i].state       = SOCK_FREE;
        socks[i].slot_id     = (uint8_t)i;
        socks[i].peer_sock_id = 0xFFu;
        socks[i]._pad        = 0;
        socks[i].path[0]     = '\0';
    }

    if (pflocal_shmem_vaddr) {
        for (uint32_t i = 0; i < PFLOCAL_MAX_SOCKS; i++)
            ring_init((uint8_t)i);
        microkit_dbg_puts("[pflocal] init: 16 slots, shmem mapped\n");
    } else {
        microkit_dbg_puts("[pflocal] init: 16 slots, shmem not-mapped\n");
    }
}

void notified(microkit_channel ch) { (void)ch; }

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg)
{
    (void)ch; (void)msg;
    uint32_t op      = (uint32_t)microkit_mr_get(0);
    uint32_t sock_id = (uint32_t)microkit_mr_get(1);

    switch (op) {

    /* OP_PFLOCAL_SOCKET: allocate a new local socket.
     * → MR0=ok(0)/err, MR1=sock_id, MR2=slot_offset */
    case OP_PFLOCAL_SOCKET: {
        uint32_t id = PFLOCAL_MAX_SOCKS;
        for (uint32_t i = 0; i < PFLOCAL_MAX_SOCKS; i++) {
            if (socks[i].state == SOCK_FREE) { id = i; break; }
        }
        if (id == PFLOCAL_MAX_SOCKS) {
            /* No free slots */
            microkit_mr_set(0, 0xFDu);  /* PFLOCAL_ERR_FULL */
            return microkit_msginfo_new(0, 1);
        }
        socks[id].state        = SOCK_CREATED;
        socks[id].slot_id      = (uint8_t)id;
        socks[id].peer_sock_id = 0xFFu;
        socks[id].path[0]      = '\0';
        if (pflocal_shmem_vaddr)
            ring_init((uint8_t)id);
        microkit_mr_set(0, 0u);
        microkit_mr_set(1, id);
        microkit_mr_set(2, id * PFLOCAL_SLOT_SIZE);
        return microkit_msginfo_new(0, 3);
    }

    /* OP_PFLOCAL_BIND: MR1=sock_id; path is null-terminated at pflocal_shmem[0].
     * → MR0=ok */
    case OP_PFLOCAL_BIND: {
        if (sock_id >= PFLOCAL_MAX_SOCKS || socks[sock_id].state == SOCK_FREE) {
            microkit_mr_set(0, 0xFFu);
            return microkit_msginfo_new(0, 1);
        }
        if (pflocal_shmem_vaddr) {
            const char *path = (const char *)pflocal_shmem_vaddr;
            str_ncopy(socks[sock_id].path, path, sizeof(socks[sock_id].path));
        }
        socks[sock_id].state = SOCK_BOUND;
        microkit_mr_set(0, 0u);
        return microkit_msginfo_new(0, 1);
    }

    /* OP_PFLOCAL_LISTEN: MR1=sock_id → MR0=ok */
    case OP_PFLOCAL_LISTEN: {
        if (sock_id >= PFLOCAL_MAX_SOCKS || socks[sock_id].state == SOCK_FREE) {
            microkit_mr_set(0, 0xFFu);
            return microkit_msginfo_new(0, 1);
        }
        socks[sock_id].state = SOCK_LISTENING;
        microkit_mr_set(0, 0u);
        return microkit_msginfo_new(0, 1);
    }

    /* OP_PFLOCAL_CONNECT: MR1=sock_id; path at pflocal_shmem[0].
     * Finds a SOCK_LISTENING socket with matching path and connects.
     * → MR0=ok */
    case OP_PFLOCAL_CONNECT: {
        if (sock_id >= PFLOCAL_MAX_SOCKS || socks[sock_id].state == SOCK_FREE) {
            microkit_mr_set(0, 0xFFu);
            return microkit_msginfo_new(0, 1);
        }
        if (!pflocal_shmem_vaddr) {
            microkit_mr_set(0, 0xFEu);  /* PFLOCAL_ERR_NO_SHMEM */
            return microkit_msginfo_new(0, 1);
        }
        const char *path = (const char *)pflocal_shmem_vaddr;
        int server_id = find_bound_sock(path);
        if (server_id < 0) {
            microkit_mr_set(0, 0xFEu);  /* PFLOCAL_ERR_NOENT */
            return microkit_msginfo_new(0, 1);
        }
        socks[sock_id].state        = SOCK_CONNECTED;
        socks[sock_id].peer_sock_id = (uint8_t)server_id;
        socks[(uint8_t)server_id].peer_sock_id = (uint8_t)sock_id;
        microkit_mr_set(0, 0u);
        return microkit_msginfo_new(0, 1);
    }

    /* OP_PFLOCAL_ACCEPT: MR1=sock_id (listening).
     * Returns the first connected socket whose peer == sock_id.
     * → MR0=ok, MR1=new_sock_id, MR2=peer_slot_offset */
    case OP_PFLOCAL_ACCEPT: {
        if (sock_id >= PFLOCAL_MAX_SOCKS || socks[sock_id].state == SOCK_FREE) {
            microkit_mr_set(0, 0xFFu);
            return microkit_msginfo_new(0, 1);
        }
        /* Find a socket in SOCK_CONNECTED state whose peer is this server */
        uint32_t new_id = PFLOCAL_MAX_SOCKS;
        for (uint32_t i = 0; i < PFLOCAL_MAX_SOCKS; i++) {
            if (socks[i].state == SOCK_CONNECTED &&
                socks[i].peer_sock_id == (uint8_t)sock_id) {
                new_id = i;
                break;
            }
        }
        if (new_id == PFLOCAL_MAX_SOCKS) {
            microkit_mr_set(0, 0xFCu);  /* PFLOCAL_ERR_AGAIN: no pending connection */
            return microkit_msginfo_new(0, 1);
        }
        uint32_t peer_slot = (uint32_t)socks[new_id].slot_id * PFLOCAL_SLOT_SIZE;
        microkit_mr_set(0, 0u);
        microkit_mr_set(1, new_id);
        microkit_mr_set(2, peer_slot);
        return microkit_msginfo_new(0, 3);
    }

    /* OP_PFLOCAL_SEND: MR1=sock_id, MR2=src_offset (into pflocal_shmem), MR3=len.
     * Copies len bytes from pflocal_shmem+src_offset into peer's ring buffer.
     * → MR0=ok, MR1=bytes_sent */
    case OP_PFLOCAL_SEND: {
        uint32_t src_offset = (uint32_t)microkit_mr_get(2);
        uint32_t len        = (uint32_t)microkit_mr_get(3);

        if (sock_id >= PFLOCAL_MAX_SOCKS || socks[sock_id].state != SOCK_CONNECTED) {
            microkit_mr_set(0, 0xFFu);
            return microkit_msginfo_new(0, 1);
        }
        uint8_t peer_id = socks[sock_id].peer_sock_id;
        if (peer_id >= PFLOCAL_MAX_SOCKS || socks[peer_id].state == SOCK_FREE) {
            microkit_mr_set(0, 0xFBu);  /* PFLOCAL_ERR_PEER_GONE */
            return microkit_msginfo_new(0, 1);
        }
        if (!pflocal_shmem_vaddr || len == 0u) {
            microkit_mr_set(0, 0u);
            microkit_mr_set(1, 0u);
            return microkit_msginfo_new(0, 2);
        }

        ring_header_t *ring = slot_ring(socks[peer_id].slot_id);
        uint8_t       *data = slot_data(socks[peer_id].slot_id);
        const uint8_t *src  = (const uint8_t *)(pflocal_shmem_vaddr + src_offset);

        uint32_t sent = 0;
        uint32_t cap  = ring->capacity ? ring->capacity : (uint16_t)RING_DATA_SIZE;
        for (uint32_t i = 0; i < len; i++) {
            uint16_t next = (uint16_t)((ring->head + 1u) % cap);
            if (next == ring->tail) break;  /* ring full — drop remaining */
            data[ring->head] = src[i];
            ring->head = next;
            sent++;
        }
        microkit_mr_set(0, 0u);
        microkit_mr_set(1, sent);
        return microkit_msginfo_new(0, 2);
    }

    /* OP_PFLOCAL_RECV: MR1=sock_id, MR2=dst_offset (into pflocal_shmem), MR3=max_len.
     * Copies up to max_len available bytes from this socket's ring into pflocal_shmem.
     * → MR0=ok, MR1=bytes_received */
    case OP_PFLOCAL_RECV: {
        uint32_t dst_offset = (uint32_t)microkit_mr_get(2);
        uint32_t max_len    = (uint32_t)microkit_mr_get(3);

        if (sock_id >= PFLOCAL_MAX_SOCKS || socks[sock_id].state == SOCK_FREE) {
            microkit_mr_set(0, 0xFFu);
            return microkit_msginfo_new(0, 1);
        }
        if (!pflocal_shmem_vaddr || max_len == 0u) {
            microkit_mr_set(0, 0u);
            microkit_mr_set(1, 0u);
            return microkit_msginfo_new(0, 2);
        }

        ring_header_t *ring = slot_ring(socks[sock_id].slot_id);
        uint8_t       *data = slot_data(socks[sock_id].slot_id);
        uint8_t       *dst  = (uint8_t *)(pflocal_shmem_vaddr + dst_offset);

        uint32_t copied = 0;
        uint32_t cap    = ring->capacity ? ring->capacity : (uint16_t)RING_DATA_SIZE;
        while (copied < max_len && ring->tail != ring->head) {
            dst[copied] = data[ring->tail];
            ring->tail  = (uint16_t)((ring->tail + 1u) % cap);
            copied++;
        }
        microkit_mr_set(0, 0u);
        microkit_mr_set(1, copied);
        return microkit_msginfo_new(0, 2);
    }

    /* OP_PFLOCAL_CLOSE: MR1=sock_id → MR0=ok.
     * Closes the socket; if it has a connected peer, marks peer SOCK_CLOSED too. */
    case OP_PFLOCAL_CLOSE: {
        if (sock_id >= PFLOCAL_MAX_SOCKS || socks[sock_id].state == SOCK_FREE) {
            microkit_mr_set(0, 0xFFu);
            return microkit_msginfo_new(0, 1);
        }
        uint8_t peer_id = socks[sock_id].peer_sock_id;
        if (peer_id != 0xFFu && peer_id < PFLOCAL_MAX_SOCKS &&
            socks[peer_id].state != SOCK_FREE) {
            socks[peer_id].state        = SOCK_CLOSED;
            socks[peer_id].peer_sock_id = 0xFFu;
        }
        socks[sock_id].state        = SOCK_CLOSED;
        socks[sock_id].peer_sock_id = 0xFFu;
        microkit_mr_set(0, 0u);
        return microkit_msginfo_new(0, 1);
    }

    /* OP_PFLOCAL_STATUS: MR1=sock_id → MR0=ok, MR1=state, MR2=peer_sock_id */
    case OP_PFLOCAL_STATUS: {
        if (sock_id >= PFLOCAL_MAX_SOCKS) {
            microkit_mr_set(0, 0xFFu);
            return microkit_msginfo_new(0, 1);
        }
        microkit_mr_set(0, 0u);
        microkit_mr_set(1, socks[sock_id].state);
        microkit_mr_set(2, socks[sock_id].peer_sock_id);
        return microkit_msginfo_new(0, 3);
    }

    default:
        microkit_mr_set(0, 0xFFu);
        return microkit_msginfo_new(0, 1);
    }
}
