/*
 * agentOS Time-Partitioning Scheduler Protection Domain
 *
 * Passive PD (priority 250). Enforces seL4 MCS CPU budget windows.
 *
 * E5-S8: migrated from Microkit to raw seL4 IPC.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "contracts/time_partition_contract.h"
#include "prio_inherit.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef MICROKIT_SC_CAP_BASE
#  ifndef AGENTOS_TEST_HOST
#    include <sel4/sel4.h>
#  endif
#  define TP_MCS_AVAILABLE 1
#else
#  define TP_MCS_AVAILABLE 0
#endif

#define MAX_PDS    32
#define MAX_CLASSES 8

#define CLASS_GPU_WORKER   0
#define CLASS_MESH_AGENT   1
#define CLASS_WATCHDOG     2
#define CLASS_INIT_AGENT   3
#define CLASS_BACKGROUND   4
#define CLASS_CUSTOM       5

#define OP_TP_CONFIGURE    0xD0
#define OP_TP_SET_POLICY   0xD1
#define OP_TP_GET_POLICY   0xD2
#define OP_TP_SUSPEND      0xD3
#define OP_TP_QUERY        0xD4
#define OP_TP_TICK         0xD5
#define OP_TP_RESET        0xD6

typedef struct { uint32_t budget_us, period_us; char name[32]; } TPClass;
typedef struct {
    uint32_t pd_id; uint8_t class_id; bool registered;
    uint32_t remaining_budget_us, ticks_this_period; bool suspended;
} TPEntry;

static TPClass tp_classes[MAX_CLASSES] = {
    [CLASS_GPU_WORKER]  = { 5000, 10000, "gpu_worker" },
    [CLASS_MESH_AGENT]  = { 2000, 10000, "mesh_agent" },
    [CLASS_WATCHDOG]    = { 1000,  5000, "watchdog"   },
    [CLASS_INIT_AGENT]  = {  500, 10000, "init_agent" },
    [CLASS_BACKGROUND]  = {  200, 10000, "background" },
    [CLASS_CUSTOM]      = {    0,     0, "custom"     },
};
static TPEntry tp_table[MAX_PDS];
static uint32_t tp_count = 0;
static uint64_t tick_count = 0;

#if TP_MCS_AVAILABLE
static seL4_CPtr sc_cap_for_pd(uint32_t pd_id) {
    return (seL4_CPtr)(MICROKIT_SC_CAP_BASE + pd_id);
}
static void tp_enforce_policy(TPEntry *e) {
    if (!e || !e->registered) return;
    const TPClass *cls = &tp_classes[e->class_id];
    if (cls->budget_us == 0 || cls->period_us == 0) return;
#ifndef AGENTOS_TEST_HOST
    seL4_CPtr sc = sc_cap_for_pd(e->pd_id);
    uint64_t period_ns = (uint64_t)cls->period_us * 1000ULL;
    uint64_t budget_ns = (uint64_t)cls->budget_us * 1000ULL;
    seL4_Error err = seL4_SchedContext_SetPeriodAndBudget(sc, period_ns, budget_ns);
    if (err != seL4_NoError)
        sel4_dbg_puts("[time_partition] WARNING: seL4_SchedContext_SetPeriodAndBudget failed\n");
#endif
}
static void tp_suspend_pd(TPEntry *e) {
    if (!e || !e->registered || e->suspended) return;
#ifndef AGENTOS_TEST_HOST
    seL4_SchedContext_SetPeriodAndBudget(sc_cap_for_pd(e->pd_id),
        (uint64_t)tp_classes[e->class_id].period_us * 1000ULL, 0);
#endif
    e->suspended = true;
}
static void tp_restore_pd(TPEntry *e) {
    if (!e || !e->registered || !e->suspended) return;
    tp_enforce_policy(e);
    e->suspended = false;
    e->remaining_budget_us = tp_classes[e->class_id].budget_us;
}
#else
static void tp_enforce_policy(TPEntry *e) { (void)e; }
static void tp_suspend_pd(TPEntry *e) { if (e) e->suspended = true; }
static void tp_restore_pd(TPEntry *e) {
    if (e) { e->suspended = false;
        e->remaining_budget_us = e->class_id < MAX_CLASSES ? tp_classes[e->class_id].budget_us : 0; }
}
#endif

static TPEntry *find_pd(uint32_t pd_id) {
    for (uint32_t i = 0; i < tp_count; i++)
        if (tp_table[i].registered && tp_table[i].pd_id == pd_id) return &tp_table[i];
    return (void *)0;
}
static TPEntry *alloc_pd(uint32_t pd_id) {
    if (tp_count >= MAX_PDS) return (void *)0;
    TPEntry *e = &tp_table[tp_count++];
    memset(e, 0, sizeof(*e)); e->pd_id = pd_id; return e;
}

static void handle_tick(uint32_t quantum_us) {
    tick_count++;
    for (uint32_t i = 0; i < tp_count; i++) {
        TPEntry *e = &tp_table[i];
        if (!e->registered) continue;
        const TPClass *cls = &tp_classes[e->class_id];
        uint32_t tpp = cls->period_us / (quantum_us > 0 ? quantum_us : 500);
        if (tpp < 1) tpp = 1;
        e->ticks_this_period++;
        if (e->ticks_this_period >= tpp) {
            e->ticks_this_period = 0;
            if (e->suspended) tp_restore_pd(e);
            else e->remaining_budget_us = cls->budget_us;
        }
        if (!TP_MCS_AVAILABLE) {
            if (e->remaining_budget_us > quantum_us) e->remaining_budget_us -= quantum_us;
            else if (!e->suspended) tp_suspend_pd(e);
        }
    }
}

/* msg helpers */
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

static uint32_t h_configure(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t pd_id = msg_u32(req, 0);
    uint8_t  cid   = (uint8_t)(msg_u32(req, 4) < MAX_CLASSES ? msg_u32(req, 4) : CLASS_BACKGROUND);
    TPEntry *e = find_pd(pd_id); if (!e) e = alloc_pd(pd_id);
    if (!e) { rep_u32(rep, 0, 0); rep->length = 4; return SEL4_ERR_NO_MEM; }
    e->registered = true; e->class_id = cid;
    e->remaining_budget_us = tp_classes[cid].budget_us; e->suspended = false;
    tp_enforce_policy(e);
    rep_u32(rep, 0, 1); rep->length = 4; return SEL4_ERR_OK;
}

static uint32_t h_set_policy(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint8_t cid = (uint8_t)(msg_u32(req, 0) < MAX_CLASSES ? msg_u32(req, 0) : CLASS_CUSTOM);
    uint32_t bud = msg_u32(req, 4), per = msg_u32(req, 8);
    if (bud) tp_classes[cid].budget_us = bud;
    if (per) tp_classes[cid].period_us = per;
    for (uint32_t i = 0; i < tp_count; i++)
        if (tp_table[i].registered && tp_table[i].class_id == cid) tp_enforce_policy(&tp_table[i]);
    rep_u32(rep, 0, 1); rep->length = 4; return SEL4_ERR_OK;
}

static uint32_t h_get_policy(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint8_t cid = (uint8_t)(msg_u32(req, 0) < MAX_CLASSES ? msg_u32(req, 0) : 0);
    rep_u32(rep, 0, tp_classes[cid].budget_us);
    rep_u32(rep, 4, tp_classes[cid].period_us);
    rep->length = 8; return SEL4_ERR_OK;
}

static uint32_t h_suspend(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    TPEntry *e = find_pd(msg_u32(req, 0)); if (e) tp_suspend_pd(e);
    rep_u32(rep, 0, e ? 1u : 0u); rep->length = 4; return SEL4_ERR_OK;
}

static uint32_t h_query(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    TPEntry *e = find_pd(msg_u32(req, 0));
    rep_u32(rep, 0, e ? e->remaining_budget_us : 0u);
    rep_u32(rep, 4, e ? (uint32_t)e->class_id : 0xFFu);
    rep_u32(rep, 8, e ? (uint32_t)e->suspended : 0u);
    rep->length = 12; return SEL4_ERR_OK;
}

static uint32_t h_tick(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t q = msg_u32(req, 0); if (!q) q = 500;
    handle_tick(q);
    rep_u32(rep, 0, (uint32_t)(tick_count & 0xFFFFFFFF));
    rep->length = 4; return SEL4_ERR_OK;
}

static uint32_t h_reset(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    memset(tp_table, 0, sizeof(tp_table)); tp_count = 0; tick_count = 0;
    rep_u32(rep, 0, 1); rep->length = 4; return SEL4_ERR_OK;
}

void time_partition_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    memset(tp_table, 0, sizeof(tp_table));
    sel4_dbg_puts("[time_partition] Scheduling policies initialized\n");

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, OP_TP_CONFIGURE,  h_configure,  (void *)0);
    sel4_server_register(&srv, OP_TP_SET_POLICY, h_set_policy, (void *)0);
    sel4_server_register(&srv, OP_TP_GET_POLICY, h_get_policy, (void *)0);
    sel4_server_register(&srv, OP_TP_SUSPEND,    h_suspend,    (void *)0);
    sel4_server_register(&srv, OP_TP_QUERY,      h_query,      (void *)0);
    sel4_server_register(&srv, OP_TP_TICK,       h_tick,       (void *)0);
    sel4_server_register(&srv, OP_TP_RESET,      h_reset,      (void *)0);
    sel4_server_run(&srv);
}
