/*
 * term_server.c — PTY multiplexer and line discipline for agentOS
 *
 * HURD-equivalent: term translator
 * Priority: 150 (passive)
 *
 * Opcodes: OP_TERM_OPENPTY/RESIZE/WRITE/READ/CLOSEPTY/STATUS
 *
 * E5-S8: migrated from Microkit to raw seL4 IPC.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "agentos.h"
#include "sel4_server.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ── Configuration ─────────────────────────────────────────────────────── */
#define TERM_MAX_PTYS    4u
#define TERM_SLOT_SIZE   0x1000u
#define TERM_HALF_SIZE   0x800u
#define TERM_RING_HDR    16u
#define TERM_RING_CAP    (TERM_HALF_SIZE - TERM_RING_HDR)
#define TERM_RING_MAGIC  0x54544559u

#define TERM_DEFAULT_ROWS 24u
#define TERM_DEFAULT_COLS 80u

/* ── Shared memory ─────────────────────────────────────────────────────── */
uintptr_t term_shmem_vaddr;

/* ── PTY state ─────────────────────────────────────────────────────────── */
typedef struct {
    bool     open;
    uint8_t  rows;
    uint8_t  cols;
    uint8_t  _pad;
    uint32_t slave_pid;
} pty_state_t;

static pty_state_t ptys[TERM_MAX_PTYS];

/* ── Ring buffer helpers ───────────────────────────────────────────────── */
typedef struct {
    uint32_t head;
    uint32_t tail;
    uint32_t cap;
    uint32_t magic;
} term_ring_t;

static inline term_ring_t *m2s_ring(uint32_t pty_id)
{
    return (term_ring_t *)(term_shmem_vaddr
                           + (uintptr_t)pty_id * TERM_SLOT_SIZE);
}

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

static uint32_t ring_avail(term_ring_t *r) { return r->tail - r->head; }
static uint32_t ring_free(term_ring_t *r)  { return r->cap - ring_avail(r); }

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

/* ── Outbound endpoint to proc_server (resolved at init time) ──────────── */
static seL4_CPtr g_proc_ep = 0;
static seL4_CPtr g_ns_ep   = 0;

/* ── Line discipline ──────────────────────────────────────────────────── */
static void line_disc_write(uint32_t pty_id, const uint8_t *buf, uint32_t len)
{
    term_ring_t *m2s = m2s_ring(pty_id);
    term_ring_t *s2m = s2m_ring(pty_id);

    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = buf[i];

        /* Ctrl-C → SIGINT (signal 2) to slave_pid via proc_server */
        if (c == 0x03u && ptys[pty_id].slave_pid != 0 && g_proc_ep != 0) {
#ifndef AGENTOS_TEST_HOST
            seL4_SetMR(0, OP_PROC_KILL);
            seL4_SetMR(1, ptys[pty_id].slave_pid);
            seL4_SetMR(2, 2u);
            seL4_MessageInfo_t _i = seL4_MessageInfo_new(OP_PROC_KILL, 0, 0, 3);
            (void)seL4_Call(g_proc_ep, _i);
#endif
            continue;
        }
        if (c == '\r') c = '\n';
        ring_push(m2s, &c, 1);
        ring_push(s2m, &c, 1);
    }
}

/* ── msg data helpers ──────────────────────────────────────────────────── */
static inline uint32_t msg_u32(const sel4_msg_t *m, uint32_t off) {
    uint32_t v = 0;
    if (off + 4u <= SEL4_MSG_DATA_BYTES) {
        v  = (uint32_t)m->data[off];
        v |= (uint32_t)m->data[off+1] << 8;
        v |= (uint32_t)m->data[off+2] << 16;
        v |= (uint32_t)m->data[off+3] << 24;
    }
    return v;
}
static inline void rep_u32(sel4_msg_t *m, uint32_t off, uint32_t v) {
    if (off + 4u <= SEL4_MSG_DATA_BYTES) {
        m->data[off]   = (uint8_t)v;
        m->data[off+1] = (uint8_t)(v >> 8);
        m->data[off+2] = (uint8_t)(v >> 16);
        m->data[off+3] = (uint8_t)(v >> 24);
    }
}

/* ── Opcode handlers ───────────────────────────────────────────────────── */

static uint32_t handle_openpty(sel4_badge_t b, const sel4_msg_t *req,
                                sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)req; (void)ctx;
    uint32_t pty_id = TERM_MAX_PTYS;
    for (uint32_t i = 0; i < TERM_MAX_PTYS; i++) {
        if (!ptys[i].open) { pty_id = i; break; }
    }
    if (pty_id == TERM_MAX_PTYS) { rep_u32(rep, 0, 0); rep->length = 4; return SEL4_ERR_NO_MEM; }

    ring_init(m2s_ring(pty_id));
    ring_init(s2m_ring(pty_id));
    ptys[pty_id].open      = true;
    ptys[pty_id].rows      = TERM_DEFAULT_ROWS;
    ptys[pty_id].cols      = TERM_DEFAULT_COLS;
    ptys[pty_id].slave_pid = 0;

    rep_u32(rep, 0,  1u);
    rep_u32(rep, 4,  pty_id);
    rep_u32(rep, 8,  (uint32_t)(pty_id * TERM_SLOT_SIZE));
    rep_u32(rep, 12, (uint32_t)(pty_id * TERM_SLOT_SIZE + TERM_HALF_SIZE));
    rep->length = 16;
    return SEL4_ERR_OK;
}

static uint32_t handle_resize(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)ctx;
    uint32_t pty_id = msg_u32(req, 0);
    uint32_t rows   = msg_u32(req, 4);
    uint32_t cols   = msg_u32(req, 8);
    if (pty_id >= TERM_MAX_PTYS || !ptys[pty_id].open) {
        rep_u32(rep, 0, 0); rep->length = 4; return SEL4_ERR_NOT_FOUND;
    }
    ptys[pty_id].rows = (uint8_t)(rows & 0xFF);
    ptys[pty_id].cols = (uint8_t)(cols & 0xFF);
    rep_u32(rep, 0, 1); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t handle_write(sel4_badge_t b, const sel4_msg_t *req,
                              sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)ctx;
    uint32_t pty_id = msg_u32(req, 0);
    uint32_t offset = msg_u32(req, 4);
    uint32_t len    = msg_u32(req, 8);
    if (pty_id >= TERM_MAX_PTYS || !ptys[pty_id].open) {
        rep_u32(rep, 0, 0); rep->length = 4; return SEL4_ERR_NOT_FOUND;
    }
    const uint8_t *src = (const uint8_t *)(term_shmem_vaddr + offset);
    line_disc_write(pty_id, src, len);
    rep_u32(rep, 0, 1); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t handle_read(sel4_badge_t b, const sel4_msg_t *req,
                             sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)ctx;
    uint32_t pty_id = msg_u32(req, 0);
    uint32_t offset = msg_u32(req, 4);
    uint32_t max    = msg_u32(req, 8);
    if (pty_id >= TERM_MAX_PTYS || !ptys[pty_id].open) {
        rep_u32(rep, 0, 0); rep->length = 4; return SEL4_ERR_NOT_FOUND;
    }
    uint8_t *dst = (uint8_t *)(term_shmem_vaddr + offset);
    uint32_t got = ring_pop(s2m_ring(pty_id), dst, max);
    rep_u32(rep, 0, 1);
    rep_u32(rep, 4, got);
    rep->length = 8;
    return SEL4_ERR_OK;
}

static uint32_t handle_closepty(sel4_badge_t b, const sel4_msg_t *req,
                                 sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)ctx;
    uint32_t pty_id = msg_u32(req, 0);
    if (pty_id >= TERM_MAX_PTYS || !ptys[pty_id].open) {
        rep_u32(rep, 0, 0); rep->length = 4; return SEL4_ERR_NOT_FOUND;
    }
    m2s_ring(pty_id)->magic = 0;
    s2m_ring(pty_id)->magic = 0;
    ptys[pty_id].open      = false;
    ptys[pty_id].slave_pid = 0;
    rep_u32(rep, 0, 1); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t handle_status(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)req; (void)ctx;
    uint32_t active = 0;
    for (uint32_t i = 0; i < TERM_MAX_PTYS; i++)
        if (ptys[i].open) active++;
    rep_u32(rep, 0, 1);
    rep_u32(rep, 4, active);
    rep->length = 8;
    return SEL4_ERR_OK;
}

/* ── Main entry point ──────────────────────────────────────────────────── */

void term_server_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    g_ns_ep = ns_ep;
    /* resolve proc_server lazily: try ns lookup if ns_ep is valid */
    /* In test builds g_proc_ep stays 0 (safe) */
    for (uint32_t i = 0; i < TERM_MAX_PTYS; i++) {
        ptys[i].open      = false;
        ptys[i].rows      = TERM_DEFAULT_ROWS;
        ptys[i].cols      = TERM_DEFAULT_COLS;
        ptys[i].slave_pid = 0;
    }
    sel4_dbg_puts("[term_server] PTY multiplexer ready (4 PTY pairs, 16KB shmem)\n");

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, OP_TERM_OPENPTY,  handle_openpty,  (void *)0);
    sel4_server_register(&srv, OP_TERM_RESIZE,   handle_resize,   (void *)0);
    sel4_server_register(&srv, OP_TERM_WRITE,    handle_write,    (void *)0);
    sel4_server_register(&srv, OP_TERM_READ,     handle_read,     (void *)0);
    sel4_server_register(&srv, OP_TERM_CLOSEPTY, handle_closepty, (void *)0);
    sel4_server_register(&srv, OP_TERM_STATUS,   handle_status,   (void *)0);
    sel4_server_run(&srv);
}
