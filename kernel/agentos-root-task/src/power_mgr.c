/*
 * agentOS Power Manager Protection Domain
 *
 * Passive PD (priority 240). Implements dynamic voltage/frequency scaling
 * (DVFS) for sparky GB10 under simulated thermal load, and integrates with
 * the time_partition PD to adjust gpu_worker CPU budgets when the die
 * temperature exceeds the configurable throttle threshold.
 *
 * Messages (MR0 opcode via sel4_msg_t.data[0..3]):
 *   OP_PWR_STATUS      (0x90): → temp_mC, freq_khz, throttled, tdp_mW
 *   OP_PWR_SET_POLICY  (0x91): MR1=threshold_mC → ok
 *   OP_PWR_THROTTLE    (0x92): force throttle → ok
 *   OP_PWR_UNTHROTTLE  (0x93): force unthrottle → ok
 *   OP_PWR_GET_HISTORY (0x94): → MR0=count
 *   OP_PWR_RESET       (0x95): reset all state → ok
 *   OP_TP_TICK         (0xD5): MR1=active_slots → advance thermal model
 *
 * Build flag: -DAGENTOS_POWER_MGR activates full logic; without it the PD
 * compiles to a no-op stub.
 *
 * E5-S8: migrated from Microkit to raw seL4 IPC.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "contracts/power_mgr_contract.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef AGENTOS_POWER_MGR

/* ── Configuration ─────────────────────────────────────────────────────── */

#define TEMP_MIN_mC               25000
#define TEMP_MAX_mC               95000
#define TEMP_INIT_mC              45000
#define TEMP_DEFAULT_THRESHOLD_mC 80000
#define TEMP_HYSTERESIS_mC         5000

#define FREQ_NOMINAL_kHz          2800000U
#define FREQ_THROTTLED_kHz        1400000U

#define TDP_NOMINAL_mW             60000U
#define TDP_THROTTLED_mW           30000U

#define HIST_SIZE   16

/* ── Opcodes ────────────────────────────────────────────────────────────── */

#define OP_PWR_STATUS       0x90
#define OP_PWR_SET_POLICY   0x91
#define OP_PWR_THROTTLE     0x92
#define OP_PWR_UNTHROTTLE   0x93
#define OP_PWR_GET_HISTORY  0x94
#define OP_PWR_RESET        0x95
#define OP_TP_TICK_PWR      0xD5   /* controller tick dispatch */

/* ── Data structures ─────────────────────────────────────────────────── */

typedef struct {
    uint32_t ts_ticks;
    int32_t  temp_mC;
    uint32_t freq_khz;
    uint32_t throttled;
} PwrSample;

uintptr_t power_ring_vaddr;

/* ── Module state ─────────────────────────────────────────────────────── */

static int32_t  sim_temp_mC       = TEMP_INIT_mC;
static uint32_t active_slots      = 0;
static bool     throttled_state   = false;
static uint32_t thermal_threshold = TEMP_DEFAULT_THRESHOLD_mC;
static uint64_t tick_count        = 0;
static uint32_t hist_head         = 0;

/* ── Channel to time_partition (resolved lazily) ─────────────────────── */

static seL4_CPtr g_tp_ep = 0;

/* ── Internal helpers ─────────────────────────────────────────────────── */

static PwrSample *pwr_ring(void) {
    return (PwrSample *)(uintptr_t)power_ring_vaddr;
}

static void push_sample(void) {
    if (!power_ring_vaddr) return;
    PwrSample *s  = &pwr_ring()[hist_head % HIST_SIZE];
    s->ts_ticks   = (uint32_t)(tick_count & 0xFFFFFFFFU);
    s->temp_mC    = sim_temp_mC;
    s->freq_khz   = throttled_state ? FREQ_THROTTLED_kHz : FREQ_NOMINAL_kHz;
    s->throttled  = throttled_state ? 1U : 0U;
    hist_head++;
}

static void tp_set_gpu_period(uint32_t period_us) {
#ifdef AGENTOS_POWER_MGR_TP_WIRED
    if (g_tp_ep != 0) {
#ifndef AGENTOS_TEST_HOST
        seL4_SetMR(0, 0xD1u);  /* OP_TP_SET_POLICY */
        seL4_SetMR(1, 0u);     /* CLASS_GPU_WORKER */
        seL4_SetMR(2, 0u);     /* budget_us: 0 = leave unchanged */
        seL4_SetMR(3, period_us);
        seL4_MessageInfo_t _i = seL4_MessageInfo_new(0xD1u, 0, 0, 4);
        (void)seL4_Call(g_tp_ep, _i);
#endif
    }
#else
    (void)period_us;
    sel4_dbg_puts("[power_mgr] STUB: would set gpu_worker period_us via time_partition\n");
#endif
}

static void do_tick(uint32_t slots) {
    tick_count++;
    active_slots = slots;

    int32_t delta = (int32_t)(slots * 1500U) - 2000;
    sim_temp_mC  += delta;
    if (sim_temp_mC < TEMP_MIN_mC) sim_temp_mC = TEMP_MIN_mC;
    if (sim_temp_mC > TEMP_MAX_mC) sim_temp_mC = TEMP_MAX_mC;

    bool was_throttled = throttled_state;

    if (!throttled_state && sim_temp_mC > (int32_t)thermal_threshold) {
        throttled_state = true;
    } else if (throttled_state &&
               sim_temp_mC < (int32_t)(thermal_threshold - TEMP_HYSTERESIS_mC)) {
        throttled_state = false;
    }

    if (!was_throttled && throttled_state) {
        sel4_dbg_puts("[power_mgr] THROTTLE: temp > threshold, doubling gpu_worker period\n");
        tp_set_gpu_period(20000U);
    } else if (was_throttled && !throttled_state) {
        sel4_dbg_puts("[power_mgr] UNTHROTTLE: temp < hysteresis, restoring gpu_worker period\n");
        tp_set_gpu_period(10000U);
    }

    push_sample();
}

static void reset_all(void) {
    sim_temp_mC       = TEMP_INIT_mC;
    active_slots      = 0;
    throttled_state   = false;
    thermal_threshold = TEMP_DEFAULT_THRESHOLD_mC;
    tick_count        = 0;
    hist_head         = 0;
    if (power_ring_vaddr)
        memset((void *)(uintptr_t)power_ring_vaddr, 0, HIST_SIZE * sizeof(PwrSample));
}

/* ── msg helpers ────────────────────────────────────────────────────── */

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

/* ── IPC handlers ───────────────────────────────────────────────────── */

static uint32_t h_status(sel4_badge_t b, const sel4_msg_t *req,
                           sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    rep_u32(rep, 0,  (uint32_t)sim_temp_mC);
    rep_u32(rep, 4,  throttled_state ? FREQ_THROTTLED_kHz : FREQ_NOMINAL_kHz);
    rep_u32(rep, 8,  throttled_state ? 1U : 0U);
    rep_u32(rep, 12, throttled_state ? TDP_THROTTLED_mW : TDP_NOMINAL_mW);
    rep->length = 16;
    return SEL4_ERR_OK;
}

static uint32_t h_set_policy(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t thresh = msg_u32(req, 4);
    if (thresh >= (uint32_t)TEMP_MIN_mC && thresh <= (uint32_t)TEMP_MAX_mC)
        thermal_threshold = thresh;
    rep_u32(rep, 0, 1U);
    rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_throttle(sel4_badge_t b, const sel4_msg_t *req,
                             sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    if (!throttled_state) {
        throttled_state = true;
        sel4_dbg_puts("[power_mgr] FORCE THROTTLE\n");
        tp_set_gpu_period(20000U);
    }
    rep_u32(rep, 0, 1U);
    rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_unthrottle(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    if (throttled_state) {
        throttled_state = false;
        sel4_dbg_puts("[power_mgr] FORCE UNTHROTTLE\n");
        tp_set_gpu_period(10000U);
    }
    rep_u32(rep, 0, 1U);
    rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_get_history(sel4_badge_t b, const sel4_msg_t *req,
                                sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    uint32_t count = (tick_count < HIST_SIZE)
                   ? (uint32_t)tick_count
                   : (uint32_t)HIST_SIZE;
    rep_u32(rep, 0, count);
    rep_u32(rep, 4, hist_head % HIST_SIZE);
    rep->length = 8;
    return SEL4_ERR_OK;
}

static uint32_t h_reset(sel4_badge_t b, const sel4_msg_t *req,
                          sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    reset_all();
    sel4_dbg_puts("[power_mgr] state reset\n");
    rep_u32(rep, 0, 1U);
    rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_tick(sel4_badge_t b, const sel4_msg_t *req,
                         sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t slots = msg_u32(req, 4);
    do_tick(slots);
    rep_u32(rep, 0, (uint32_t)(tick_count & 0xFFFFFFFFU));
    rep->length = 4;
    return SEL4_ERR_OK;
}

void power_mgr_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    agentos_log_boot("power_mgr");
    reset_all();
    sel4_dbg_puts("[power_mgr] DVFS thermal manager initialized\n");
    sel4_dbg_puts("[power_mgr] Threshold: 80C  Hysteresis: 5C  Nominal: 2.8 GHz\n");
#ifdef AGENTOS_POWER_MGR_TP_WIRED
    sel4_dbg_puts("[power_mgr] time_partition channel: WIRED\n");
#else
    sel4_dbg_puts("[power_mgr] time_partition channel: STUB\n");
#endif

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, OP_PWR_STATUS,      h_status,      (void *)0);
    sel4_server_register(&srv, OP_PWR_SET_POLICY,  h_set_policy,  (void *)0);
    sel4_server_register(&srv, OP_PWR_THROTTLE,    h_throttle,    (void *)0);
    sel4_server_register(&srv, OP_PWR_UNTHROTTLE,  h_unthrottle,  (void *)0);
    sel4_server_register(&srv, OP_PWR_GET_HISTORY, h_get_history, (void *)0);
    sel4_server_register(&srv, OP_PWR_RESET,       h_reset,       (void *)0);
    sel4_server_register(&srv, OP_TP_TICK_PWR,     h_tick,        (void *)0);
    sel4_server_run(&srv);
}

#else /* !AGENTOS_POWER_MGR — no-op stub ──────────────────────────────── */

void power_mgr_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)my_ep; (void)ns_ep;
    sel4_dbg_puts("[power_mgr] stub (build with AGENTOS_POWER_MGR=1 to enable)\n");
    /* passive stub: block forever */
    for (;;) {
#ifndef AGENTOS_TEST_HOST
        seL4_Word badge = 0;
        (void)seL4_Recv(my_ep, &badge);
        seL4_MessageInfo_t rep = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, 0U);
        seL4_Reply(rep);
#else
        break;
#endif
    }
}

#endif /* AGENTOS_POWER_MGR */
