/*
 * term_server.c — PTY multiplexer and line discipline for agentOS
 *
 * HURD-equivalent: term translator
 * Priority: 150 (passive)
 *
 * Provides up to TERM_MAX_PTYS pseudo-terminal (PTY) pairs.  Each PTY has
 * a master side (faces the console bridge / xterm.js via WebSocket) and a
 * slave side (faces the running process).
 *
 * Each PTY pair occupies two 4KB slots in term_shmem (16KB total = 4 PTY
 * pairs × 2 directions × 2KB each):
 *   term_shmem slot layout (per PTY, 4KB each):
 *     master→slave  bytes [0x000..0x7FF]   2KB ring
 *     slave→master  bytes [0x800..0xFFF]   2KB ring
 *   Ring header: uint32_t head, tail, cap, magic (16 bytes)
 *
 * Line discipline (minimal):
 *   - Echo characters written on master back to slave output ring
 *   - Translate \r → \n on master input
 *   - SIGINT on Ctrl-C (0x03): notify proc_server OP_PROC_KILL signal=2
 *
 * Channel assignments:
 *   id=0: receives PPC from controller (CH_TERM_SERVER = 43)
 *
 * Opcodes (all in agentos.h):
 *   OP_TERM_OPENPTY   0xA0  → ok + pty_id + master_offset + slave_offset
 *   OP_TERM_RESIZE    0xA1  MR1=pty_id MR2=rows MR3=cols → ok
 *   OP_TERM_WRITE     0xA2  MR1=pty_id MR2=shmem_offset MR3=len → ok
 *   OP_TERM_READ      0xA3  MR1=pty_id MR2=shmem_offset MR3=max → ok+len
 *   OP_TERM_CLOSEPTY  0xA4  MR1=pty_id → ok
 *   OP_TERM_STATUS    0xA5  → ok + active_ptys
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "agentos.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ── Configuration ─────────────────────────────────────────────────────── */
#define TERM_MAX_PTYS    4u
#define TERM_SLOT_SIZE   0x1000u   /* 4KB per PTY in term_shmem */
#define TERM_HALF_SIZE   0x800u    /* 2KB per direction (master→slave / slave→master) */
#define TERM_RING_HDR    16u       /* bytes for ring header */
#define TERM_RING_CAP    (TERM_HALF_SIZE - TERM_RING_HDR)  /* 2032 bytes usable */
#define TERM_RING_MAGIC  0x54544559u  /* "YTTT" */

#define TERM_DEFAULT_ROWS 24u
#define TERM_DEFAULT_COLS 80u

/* ── Shared memory ─────────────────────────────────────────────────────── */
uintptr_t term_shmem_vaddr;  /* set by Microkit linker (16KB) */

/* ── PTY state ─────────────────────────────────────────────────────────── */
typedef struct {
    bool     open;
    uint8_t  rows;
    uint8_t  cols;
    uint8_t  _pad;
    uint32_t slave_pid;   /* process holding slave end */
} pty_state_t;

static pty_state_t ptys[TERM_MAX_PTYS];

/* ── Ring buffer helpers ───────────────────────────────────────────────── */
typedef struct {
    uint32_t head;
    uint32_t tail;
    uint32_t cap;
    uint32_t magic;
} term_ring_t;

/* master→slave ring: base of PTY slot */
static inline term_ring_t *m2s_ring(uint32_t pty_id)
{
    return (term_ring_t *)(term_shmem_vaddr
                           + (uintptr_t)pty_id * TERM_SLOT_SIZE);
}

/* slave→master ring: at offset TERM_HALF_SIZE within PTY slot */
static inline term_ring_t *s2m_ring(uint32_t pty_id)
{
    return (term_ring_t *)(term_shmem_vaddr
                           + (uintptr_t)pty_id * TERM_SLOT_SIZE
                           + TERM_HALF_SIZE);
}

static inline uint8_t *ring_data(term_ring_t *r)
{
    return (uint8_t *)r + TERM_RING_HDR;
}

static void ring_init(term_ring_t *r)
{
    r->head  = 0;
    r->tail  = 0;
    r->cap   = TERM_RING_CAP;
    r->magic = TERM_RING_MAGIC;
}

static uint32_t ring_avail(term_ring_t *r)
{
    return r->tail - r->head;
}

static uint32_t ring_free(term_ring_t *r)
{
    return r->cap - ring_avail(r);
}

static uint32_t ring_push(term_ring_t *r, const uint8_t *src, uint32_t len)
{
    if (r->magic != TERM_RING_MAGIC) return 0;
    uint8_t *data = ring_data(r);
    uint32_t free_space = ring_free(r);
    if (len > free_space) len = free_space;
    for (uint32_t i = 0; i < len; i++) {
        data[r->tail % r->cap] = src[i];
        r->tail++;
    }
    return len;
}

static uint32_t ring_pop(term_ring_t *r, uint8_t *dst, uint32_t max)
{
    if (r->magic != TERM_RING_MAGIC) return 0;
    uint8_t *data  = ring_data(r);
    uint32_t avail = ring_avail(r);
    if (max > avail) max = avail;
    for (uint32_t i = 0; i < max; i++) {
        dst[i] = data[r->head % r->cap];
        r->head++;
    }
    return max;
}

/* ── Line discipline ──────────────────────────────────────────────────── */
static void line_disc_write(uint32_t pty_id, const uint8_t *buf, uint32_t len)
{
    term_ring_t *m2s = m2s_ring(pty_id);
    term_ring_t *s2m = s2m_ring(pty_id);

    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = buf[i];

        /* Ctrl-C → SIGINT (signal 2) to slave_pid via proc_server */
        if (c == 0x03u && ptys[pty_id].slave_pid != 0) {
            /* Best-effort: send OP_PROC_KILL to proc_server.
             * In Phase 1 we do not block-wait for the reply. */
            microkit_mr_set(0, OP_PROC_KILL);
            microkit_mr_set(1, ptys[pty_id].slave_pid);
            microkit_mr_set(2, 2u);  /* SIGINT */
            microkit_ppcall(CH_PROC_SERVER, microkit_msginfo_new(0, 3));
            continue;
        }

        /* CR → LF translation */
        if (c == '\r') c = '\n';

        /* write to master→slave ring */
        ring_push(m2s, &c, 1);

        /* echo back to slave→master ring */
        ring_push(s2m, &c, 1);
    }
}

/* ── IPC dispatch ─────────────────────────────────────────────────────── */
static microkit_msginfo handle_openpty(void)
{
    uint32_t pty_id = TERM_MAX_PTYS;
    for (uint32_t i = 0; i < TERM_MAX_PTYS; i++) {
        if (!ptys[i].open) { pty_id = i; break; }
    }
    if (pty_id == TERM_MAX_PTYS) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    ring_init(m2s_ring(pty_id));
    ring_init(s2m_ring(pty_id));

    ptys[pty_id].open      = true;
    ptys[pty_id].rows      = TERM_DEFAULT_ROWS;
    ptys[pty_id].cols      = TERM_DEFAULT_COLS;
    ptys[pty_id].slave_pid = 0;

    uint32_t master_off = (uint32_t)(pty_id * TERM_SLOT_SIZE);
    uint32_t slave_off  = (uint32_t)(pty_id * TERM_SLOT_SIZE + TERM_HALF_SIZE);

    microkit_mr_set(0, 1);
    microkit_mr_set(1, pty_id);
    microkit_mr_set(2, master_off);
    microkit_mr_set(3, slave_off);
    return microkit_msginfo_new(0, 4);
}

static microkit_msginfo handle_resize(void)
{
    uint32_t pty_id = (uint32_t)microkit_mr_get(1);
    uint32_t rows   = (uint32_t)microkit_mr_get(2);
    uint32_t cols   = (uint32_t)microkit_mr_get(3);

    if (pty_id >= TERM_MAX_PTYS || !ptys[pty_id].open) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }
    ptys[pty_id].rows = (uint8_t)(rows & 0xFF);
    ptys[pty_id].cols = (uint8_t)(cols & 0xFF);
    microkit_mr_set(0, 1);
    return microkit_msginfo_new(0, 1);
}

static microkit_msginfo handle_write(void)
{
    uint32_t pty_id = (uint32_t)microkit_mr_get(1);
    uint32_t offset = (uint32_t)microkit_mr_get(2);
    uint32_t len    = (uint32_t)microkit_mr_get(3);

    if (pty_id >= TERM_MAX_PTYS || !ptys[pty_id].open) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    const uint8_t *src = (const uint8_t *)(term_shmem_vaddr + offset);
    line_disc_write(pty_id, src, len);

    microkit_mr_set(0, 1);
    return microkit_msginfo_new(0, 1);
}

static microkit_msginfo handle_read(void)
{
    uint32_t pty_id = (uint32_t)microkit_mr_get(1);
    uint32_t offset = (uint32_t)microkit_mr_get(2);
    uint32_t max    = (uint32_t)microkit_mr_get(3);

    if (pty_id >= TERM_MAX_PTYS || !ptys[pty_id].open) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    uint8_t *dst = (uint8_t *)(term_shmem_vaddr + offset);
    /* Read from slave→master ring (i.e., output from the process) */
    uint32_t got = ring_pop(s2m_ring(pty_id), dst, max);

    microkit_mr_set(0, 1);
    microkit_mr_set(1, got);
    return microkit_msginfo_new(0, 2);
}

static microkit_msginfo handle_closepty(void)
{
    uint32_t pty_id = (uint32_t)microkit_mr_get(1);
    if (pty_id >= TERM_MAX_PTYS || !ptys[pty_id].open) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    /* Invalidate rings */
    m2s_ring(pty_id)->magic = 0;
    s2m_ring(pty_id)->magic = 0;

    ptys[pty_id].open      = false;
    ptys[pty_id].slave_pid = 0;

    microkit_mr_set(0, 1);
    return microkit_msginfo_new(0, 1);
}

static microkit_msginfo handle_status(void)
{
    uint32_t active = 0;
    for (uint32_t i = 0; i < TERM_MAX_PTYS; i++)
        if (ptys[i].open) active++;
    microkit_mr_set(0, 1);
    microkit_mr_set(1, active);
    return microkit_msginfo_new(0, 2);
}

/* ── Microkit entry points ────────────────────────────────────────────── */
void init(void)
{
    for (uint32_t i = 0; i < TERM_MAX_PTYS; i++) {
        ptys[i].open      = false;
        ptys[i].rows      = TERM_DEFAULT_ROWS;
        ptys[i].cols      = TERM_DEFAULT_COLS;
        ptys[i].slave_pid = 0;
    }
    microkit_dbg_puts("[term_server] PTY multiplexer ready (4 PTY pairs, 16KB shmem)\n");
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg)
{
    (void)ch;
    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {
    case OP_TERM_OPENPTY:  return handle_openpty();
    case OP_TERM_RESIZE:   return handle_resize();
    case OP_TERM_WRITE:    return handle_write();
    case OP_TERM_READ:     return handle_read();
    case OP_TERM_CLOSEPTY: return handle_closepty();
    case OP_TERM_STATUS:   return handle_status();
    default:
        microkit_dbg_puts("[term_server] unknown opcode\n");
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }
}

void notified(microkit_channel ch)
{
    (void)ch;
    /* no async notifications needed */
}
