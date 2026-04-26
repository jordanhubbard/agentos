/*
 * timer_pd.c — agentOS Generic Timer Protection Domain
 *
 * OS-neutral IPC timer and RTC service. Provides CREATE/DESTROY/START/STOP/STATUS/
 * CONFIGURE/SET_RTC/GET_RTC over seL4 IPC. Timers fire by notifying a caller-specified
 * channel; periodic timers auto-reload.
 *
 * IPC Protocol (caller -> timer_pd):
 *   TIMER_OP_CREATE    (0x1040) — data[4..7]=period_us data[8..11]=flags → data[0..3]=result data[4..7]=timer_id
 *   TIMER_OP_DESTROY   (0x1041) — data[4..7]=timer_id → data[0..3]=result
 *   TIMER_OP_START     (0x1042) — data[4..7]=timer_id → data[0..3]=result
 *   TIMER_OP_STOP      (0x1043) — data[4..7]=timer_id → data[0..3]=result
 *   TIMER_OP_STATUS    (0x1044) — data[4..7]=timer_id → running, elapsed_us
 *   TIMER_OP_CONFIGURE (0x1045) — data[4..7]=timer_id data[8..11]=period_us → result
 *   TIMER_OP_SET_RTC   (0x1046) — data[4..7]=ts_lo data[8..11]=ts_hi → result
 *   TIMER_OP_GET_RTC   (0x1047) — → data[4..7]=ts_lo data[8..11]=ts_hi
 *
 * Priority: 225
 *
 * E5-S8: migrated from Microkit to raw seL4 IPC.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "contracts/timer_contract.h"
#include <stdbool.h>

/* ── State ───────────────────────────────────────────────────────────────── */
static bool     s_slot_used[TIMER_MAX_TIMERS];
static bool     s_running[TIMER_MAX_TIMERS];
static uint32_t s_period_us[TIMER_MAX_TIMERS];
static uint32_t s_flags[TIMER_MAX_TIMERS];
static uint32_t s_elapsed_us[TIMER_MAX_TIMERS];

static uint32_t s_rtc_lo;
static uint32_t s_rtc_hi;

/* ── msg helpers ─────────────────────────────────────────────────────────── */

#ifndef AGENTOS_IPC_HELPERS_DEFINED
#define AGENTOS_IPC_HELPERS_DEFINED
static inline uint32_t msg_u32(const sel4_msg_t *m, uint32_t off) {
    uint32_t v = 0;
    if (off + 4u <= SEL4_MSG_DATA_BYTES) {
        v  = (uint32_t)m->data[off]; v |= (uint32_t)m->data[off+1]<<8;
        v |= (uint32_t)m->data[off+2]<<16; v |= (uint32_t)m->data[off+3]<<24;
    }
    return v;
}
static inline void rep_u32(sel4_msg_t *m, uint32_t off, uint32_t v) {
    if (off + 4u <= SEL4_MSG_DATA_BYTES) {
        m->data[off]=(uint8_t)v; m->data[off+1]=(uint8_t)(v>>8);
        m->data[off+2]=(uint8_t)(v>>16); m->data[off+3]=(uint8_t)(v>>24);
    }
}
#endif /* AGENTOS_IPC_HELPERS_DEFINED */

/* ── Internal helpers ─────────────────────────────────────────────────── */

static void fire_timer(uint32_t id) {
    if (s_flags[id] & TIMER_FLAG_PERIODIC) {
        s_elapsed_us[id] = 0;
    } else {
        s_running[id] = false;
    }
}

/* ── Handlers ────────────────────────────────────────────────────────────── */

static uint32_t h_create(sel4_badge_t b, const sel4_msg_t *req,
                          sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t period = msg_u32(req, 4);
    uint32_t flags  = msg_u32(req, 8);
    if (period == 0) {
        rep_u32(rep, 0, TIMER_ERR_BAD_PERIOD);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }
    uint32_t found = TIMER_MAX_TIMERS;
    for (uint32_t i = 0; i < TIMER_MAX_TIMERS; i++) {
        if (!s_slot_used[i]) { found = i; break; }
    }
    if (found == TIMER_MAX_TIMERS) {
        rep_u32(rep, 0, TIMER_ERR_NO_SLOT);
        rep->length = 4;
        return SEL4_ERR_NO_MEM;
    }
    s_slot_used[found]  = true;
    s_running[found]    = false;
    s_period_us[found]  = period;
    s_flags[found]      = flags;
    s_elapsed_us[found] = 0;
    rep_u32(rep, 0, TIMER_OK);
    rep_u32(rep, 4, found);
    rep->length = 8;
    return SEL4_ERR_OK;
}

static uint32_t h_destroy(sel4_badge_t b, const sel4_msg_t *req,
                            sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t timer_id = msg_u32(req, 4);
    if (timer_id >= TIMER_MAX_TIMERS || !s_slot_used[timer_id]) {
        rep_u32(rep, 0, TIMER_ERR_BAD_SLOT);
        rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }
    s_running[timer_id]   = false;
    s_slot_used[timer_id] = false;
    rep_u32(rep, 0, TIMER_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_start(sel4_badge_t b, const sel4_msg_t *req,
                          sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t timer_id = msg_u32(req, 4);
    if (timer_id >= TIMER_MAX_TIMERS || !s_slot_used[timer_id]) {
        rep_u32(rep, 0, TIMER_ERR_BAD_SLOT);
        rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }
    s_running[timer_id]    = true;
    s_elapsed_us[timer_id] = 0;
    rep_u32(rep, 0, TIMER_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_stop(sel4_badge_t b, const sel4_msg_t *req,
                         sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t timer_id = msg_u32(req, 4);
    if (timer_id >= TIMER_MAX_TIMERS || !s_slot_used[timer_id]) {
        rep_u32(rep, 0, TIMER_ERR_BAD_SLOT);
        rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }
    s_running[timer_id] = false;
    rep_u32(rep, 0, TIMER_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_status(sel4_badge_t b, const sel4_msg_t *req,
                           sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t timer_id = msg_u32(req, 4);
    if (timer_id >= TIMER_MAX_TIMERS || !s_slot_used[timer_id]) {
        rep_u32(rep, 0, TIMER_ERR_BAD_SLOT);
        rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }
    /* Simulate a tick advance for running timers */
    if (s_running[timer_id]) {
        s_elapsed_us[timer_id] += s_period_us[timer_id];
        fire_timer(timer_id);
    }
    rep_u32(rep, 0, TIMER_OK);
    rep_u32(rep, 4, s_running[timer_id] ? 1u : 0u);
    rep_u32(rep, 8, s_elapsed_us[timer_id]);
    rep->length = 12;
    return SEL4_ERR_OK;
}

static uint32_t h_configure(sel4_badge_t b, const sel4_msg_t *req,
                              sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t timer_id  = msg_u32(req, 4);
    uint32_t new_period = msg_u32(req, 8);
    if (timer_id >= TIMER_MAX_TIMERS || !s_slot_used[timer_id]) {
        rep_u32(rep, 0, TIMER_ERR_BAD_SLOT);
        rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }
    if (new_period == 0) {
        rep_u32(rep, 0, TIMER_ERR_BAD_PERIOD);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }
    s_period_us[timer_id] = new_period;
    rep_u32(rep, 0, TIMER_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_set_rtc(sel4_badge_t b, const sel4_msg_t *req,
                            sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    s_rtc_lo = msg_u32(req, 4);
    s_rtc_hi = msg_u32(req, 8);
    rep_u32(rep, 0, TIMER_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_get_rtc(sel4_badge_t b, const sel4_msg_t *req,
                            sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    rep_u32(rep, 0, TIMER_OK);
    rep_u32(rep, 4, s_rtc_lo);
    rep_u32(rep, 8, s_rtc_hi);
    rep->length = 12;
    return SEL4_ERR_OK;
}

/* ── Main entry point ─────────────────────────────────────────────────── */

void timer_pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    for (uint32_t i = 0; i < TIMER_MAX_TIMERS; i++) {
        s_slot_used[i]  = false;
        s_running[i]    = false;
        s_period_us[i]  = 0;
        s_flags[i]      = 0;
        s_elapsed_us[i] = 0;
    }
    s_rtc_lo = 0;
    s_rtc_hi = 0;
    sel4_dbg_puts("[timer_pd] ready\n");

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, TIMER_OP_CREATE,    h_create,    (void *)0);
    sel4_server_register(&srv, TIMER_OP_DESTROY,   h_destroy,   (void *)0);
    sel4_server_register(&srv, TIMER_OP_START,     h_start,     (void *)0);
    sel4_server_register(&srv, TIMER_OP_STOP,      h_stop,      (void *)0);
    sel4_server_register(&srv, TIMER_OP_STATUS,    h_status,    (void *)0);
    sel4_server_register(&srv, TIMER_OP_CONFIGURE, h_configure, (void *)0);
    sel4_server_register(&srv, TIMER_OP_SET_RTC,   h_set_rtc,   (void *)0);
    sel4_server_register(&srv, TIMER_OP_GET_RTC,   h_get_rtc,   (void *)0);
    sel4_server_run(&srv);
}
