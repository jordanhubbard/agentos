/*
 * agentOS PerfCounters Protection Domain
 *
 * Passive PD (priority 95). IPC latency histograms.
 *
 * E5-S8: migrated from Microkit to raw seL4 IPC.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "contracts/perf_counters_contract.h"
#include "prio_inherit.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define MAX_PD_PAIRS      64
#define HIST_BUCKETS      16
#define MAX_PENDING       16

#ifndef PERF_CYCLES_PER_NS
#  define PERF_CYCLES_PER_NS  1ULL
#endif

#define OP_PERF_BEGIN    0xC0
#define OP_PERF_END      0xC1
#define OP_PERF_QUERY    0xC2
#define OP_PERF_RESET    0xC3
#define OP_PERF_EXPORT   0xC4

static const uint64_t bucket_ns[HIST_BUCKETS] = {
    100, 200, 500, 1000, 2000, 5000, 10000, 20000,
    50000, 100000, 200000, 500000, 1000000, 5000000, 50000000,
    UINT64_MAX
};

typedef struct {
    uint32_t caller_pd, callee_pd;
    uint64_t call_count, total_ns, min_ns, max_ns;
    uint64_t buckets[HIST_BUCKETS];
} PerfEntry;

typedef struct {
    uint32_t token, caller_pd, callee_pd;
    uint64_t start_cycles;
    bool     active;
} PendingEntry;

static PerfEntry    perf_table[MAX_PD_PAIRS];
static uint32_t     perf_count  = 0;
static PendingEntry pending[MAX_PENDING];
static uint32_t     next_token  = 1;
uintptr_t perf_ring_vaddr;

static inline uint64_t read_cycles(void) {
#if defined(__riscv)
    uint64_t c; __asm__ volatile("rdcycle %0" : "=r"(c)); return c;
#elif defined(__aarch64__)
    uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v;
#else
    return 0;
#endif
}

static PerfEntry *find_or_create_entry(uint32_t c, uint32_t d) {
    for (uint32_t i = 0; i < perf_count; i++)
        if (perf_table[i].caller_pd == c && perf_table[i].callee_pd == d)
            return &perf_table[i];
    if (perf_count >= MAX_PD_PAIRS) return (void *)0;
    PerfEntry *e = &perf_table[perf_count++];
    memset(e, 0, sizeof(*e));
    e->caller_pd = c; e->callee_pd = d; e->min_ns = UINT64_MAX;
    return e;
}

static void record_latency(PerfEntry *e, uint64_t ns) {
    e->call_count++; e->total_ns += ns;
    if (ns < e->min_ns) e->min_ns = ns;
    if (ns > e->max_ns) e->max_ns = ns;
    for (int b = 0; b < HIST_BUCKETS; b++)
        if (ns < bucket_ns[b]) { e->buckets[b]++; return; }
    e->buckets[HIST_BUCKETS-1]++;
}

static uint64_t percentile(const PerfEntry *e, uint32_t pct) {
    if (e->call_count == 0) return 0;
    uint64_t target = (e->call_count * pct) / 100, cum = 0;
    for (int b = 0; b < HIST_BUCKETS-1; b++) {
        cum += e->buckets[b];
        if (cum >= target) return bucket_ns[b];
    }
    return e->max_ns;
}

#define PERF_EXPORT_RECORD_SIZE 64
static void export_to_ring(void) {
    if (!perf_ring_vaddr) return;
    uint8_t *ring = (uint8_t *)perf_ring_vaddr;
    for (uint32_t i = 0; i < perf_count; i++) {
        uint8_t *rec = ring + i * PERF_EXPORT_RECORD_SIZE;
        const PerfEntry *e = &perf_table[i];
        uint32_t *u32p = (uint32_t *)rec;
        uint64_t *u64p = (uint64_t *)(rec + 8);
        u32p[0] = e->caller_pd; u32p[1] = e->callee_pd;
        u64p[0] = e->call_count; u64p[1] = e->total_ns;
        u64p[2] = e->min_ns; u64p[3] = e->max_ns;
        u64p[4] = percentile(e, 50); u64p[5] = percentile(e, 95); u64p[6] = percentile(e, 99);
    }
    if (perf_count < MAX_PD_PAIRS)
        memset(ring + perf_count * PERF_EXPORT_RECORD_SIZE, 0, PERF_EXPORT_RECORD_SIZE);
}

/* msg helpers */
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

static uint32_t handle_begin(sel4_badge_t b, const sel4_msg_t *req,
                              sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t cpd = msg_u32(req, 0), dpd = msg_u32(req, 4);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!pending[i].active) {
            pending[i].active = true; pending[i].caller_pd = cpd;
            pending[i].callee_pd = dpd; pending[i].start_cycles = read_cycles();
            pending[i].token = next_token++;
            if (next_token == 0) next_token = 1;
            rep_u32(rep, 0, pending[i].token); rep->length = 4;
            return SEL4_ERR_OK;
        }
    }
    rep_u32(rep, 0, 0); rep->length = 4; return SEL4_ERR_OK;
}

static uint32_t handle_end(sel4_badge_t b, const sel4_msg_t *req,
                            sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t token = msg_u32(req, 0), dpd = msg_u32(req, 4);
    if (token == 0) { rep_u32(rep, 0, 0); rep_u32(rep, 4, 0); rep->length = 8; return SEL4_ERR_OK; }
    uint64_t now = read_cycles();
    for (int i = 0; i < MAX_PENDING; i++) {
        if (pending[i].active && pending[i].token == token) {
            uint64_t ns = (now - pending[i].start_cycles) / PERF_CYCLES_PER_NS;
            PerfEntry *e = find_or_create_entry(pending[i].caller_pd, dpd);
            if (e) record_latency(e, ns);
            pending[i].active = false;
            rep_u32(rep, 0, (uint32_t)(ns & 0xFFFFFFFF));
            rep_u32(rep, 4, (uint32_t)(ns >> 32));
            rep->length = 8; return SEL4_ERR_OK;
        }
    }
    rep_u32(rep, 0, 0); rep_u32(rep, 4, 0); rep->length = 8; return SEL4_ERR_OK;
}

static uint32_t handle_query(sel4_badge_t b, const sel4_msg_t *req,
                              sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t cpd = msg_u32(req, 0), dpd = msg_u32(req, 4);
    PerfEntry *e = find_or_create_entry(cpd, dpd);
    if (!e || e->call_count == 0) {
        rep_u32(rep, 0, 0); rep->length = 4; return SEL4_ERR_OK;
    }
    rep_u32(rep, 0,  (uint32_t)(e->call_count & 0xFFFFFFFF));
    rep_u32(rep, 4,  (uint32_t)(e->call_count >> 32));
    rep_u32(rep, 8,  (uint32_t)(percentile(e, 50) & 0xFFFFFFFF));
    rep_u32(rep, 12, (uint32_t)(percentile(e, 95) & 0xFFFFFFFF));
    rep_u32(rep, 16, (uint32_t)(percentile(e, 99) & 0xFFFFFFFF));
    rep->length = 20; return SEL4_ERR_OK;
}

static uint32_t handle_reset(sel4_badge_t b, const sel4_msg_t *req,
                              sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    memset(perf_table, 0, sizeof(perf_table));
    memset(pending,    0, sizeof(pending));
    perf_count = 0; next_token = 1;
    sel4_dbg_puts("[perf_counters] All histograms reset\n");
    rep_u32(rep, 0, 1); rep->length = 4; return SEL4_ERR_OK;
}

static uint32_t handle_export(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    export_to_ring();
    rep_u32(rep, 0, perf_count); rep->length = 4; return SEL4_ERR_OK;
}

void perf_counters_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    memset(perf_table, 0, sizeof(perf_table));
    memset(pending,    0, sizeof(pending));
    sel4_dbg_puts("[perf_counters] IPC latency histograms ready\n");

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, OP_PERF_BEGIN,  handle_begin,  (void *)0);
    sel4_server_register(&srv, OP_PERF_END,    handle_end,    (void *)0);
    sel4_server_register(&srv, OP_PERF_QUERY,  handle_query,  (void *)0);
    sel4_server_register(&srv, OP_PERF_RESET,  handle_reset,  (void *)0);
    sel4_server_register(&srv, OP_PERF_EXPORT, handle_export, (void *)0);
    sel4_server_run(&srv);
}
