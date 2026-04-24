/*
 * oom_killer.c — agentOS OOM Killer Protection Domain
 *
 * Passive PD (priority 245 — above all workers, below only emergency paths).
 * Subscribes to mem_profiler pressure alerts and evicts the lowest-priority
 * WASM slot when total heap usage exceeds the configured threshold.
 *
 * Eviction policy (score-based):
 *   score = (heap_used_bytes × age_ticks) / (priority + 1)
 *   Lowest score → evict first (penalises high heap, old, and low-priority slots)
 *
 * IPC protocol:
 *   OP_OOM_STATUS         (0xE2) — MR0=op → MR1=evict_count, MR2=last_slot
 *   OP_OOM_SET_THRESHOLD  (0xE3) — MR1=pct (1-99)
 *
 * Outgoing PPCs (nameserver-resolved at startup, guarded by AGENTOS_TEST_HOST):
 *   g_gpu_sched_ep → controller: OP_SLOT_EVICT (MR0=op, MR1=slot_id)
 *   g_agentfs_ep   → AgentFS: OP_SNAPSHOT_SLOT (MR0=op, MR1=slot_id)
 *
 * Thresholds:
 *   OOM_THRESHOLD_PCT = 80  — trigger eviction at 80% heap budget
 *   OOM_HARD_PCT      = 95  — hard eviction (no snapshot) at 95%
 *
 * E5-S8: migrated from Microkit to raw seL4 IPC.
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "contracts/oom_killer_contract.h"
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Op codes ─────────────────────────────────────────────────────────── */

#define OP_SLOT_EVICT         0xE0
#define OP_SNAPSHOT_SLOT      0xE1
#define OP_OOM_STATUS         0xE2
#define OP_OOM_SET_THRESHOLD  0xE3

/* ── Thresholds ───────────────────────────────────────────────────────── */

#define OOM_THRESHOLD_PCT     80
#define OOM_HARD_PCT          95
#define OOM_COOLDOWN_TICKS    100
#define OOM_MAX_SLOTS         16

/* ── Slot table (populated from mem_profiler shared ring) ─────────── */

typedef struct {
    uint8_t  slot_id;
    uint8_t  priority;
    uint64_t heap_used;
    uint64_t age_ticks;
    bool     active;
    bool     pinned;
} OomSlotInfo;

/* ── Module state ─────────────────────────────────────────────────────── */

typedef struct {
    OomSlotInfo  slots[OOM_MAX_SLOTS];
    int          slot_count;
    uint64_t     total_budget;
    uint64_t     total_used;
    uint32_t     threshold_pct;
    uint32_t     hard_pct;
    uint64_t     evict_count;
    uint8_t      last_evicted_slot;
    uint64_t     last_evict_tick;
    bool         throttled;
    uint64_t     tick;
} OomState;

static OomState s = {0};

/* ── mem_profiler shared ring ─────────────────────────────────────── */

uintptr_t mem_ring_vaddr;

#define MEM_RING_RECORD_SIZE 64

typedef struct __attribute__((packed)) {
    uint64_t ts_ns;
    uint8_t  slot_id;
    uint8_t  flags;
    uint16_t _pad;
    uint64_t heap_used;
    uint64_t heap_capacity;
    uint32_t alloc_count;
    uint32_t free_count;
    uint64_t age_ticks;
    uint8_t  priority;
    uint8_t  pinned;
    uint8_t  _pad2[6];
} MemSnapshot;

_Static_assert(sizeof(MemSnapshot) == MEM_RING_RECORD_SIZE,
               "MemSnapshot size mismatch");

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t _pad;
    uint32_t ring_head;
    uint32_t capacity;
    uint64_t total_budget;
} MemRingHeader;

/* ── Outbound endpoints (resolved lazily) ─────────────────────────── */

static seL4_CPtr g_gpu_sched_ep = 0;
static seL4_CPtr g_agentfs_ep   = 0;

/* ── Scoring function ─────────────────────────────────────────────── */

static uint64_t oom_score(const OomSlotInfo *slot) {
    if (!slot->active || slot->pinned) return UINT64_MAX;
    if (slot->heap_used == 0) return 0;
    uint64_t numerator = slot->heap_used * (slot->age_ticks + 1);
    uint64_t denom     = (uint64_t)(slot->priority + 1);
    return numerator / denom;
}

/* ── Read slot data from mem_profiler ring ────────────────────────── */

static void refresh_slot_table(void) {
    if (!mem_ring_vaddr) return;

    const MemRingHeader *hdr = (const MemRingHeader *)mem_ring_vaddr;
    if (hdr->magic != 0x4D454D52UL) return;

    s.total_budget = hdr->total_budget
        ? hdr->total_budget
        : (192ULL * 1024 * 1024 * 1024);

    const MemSnapshot *ring = (const MemSnapshot *)(
        (uint8_t *)mem_ring_vaddr + sizeof(MemRingHeader));
    uint32_t cap = hdr->capacity;
    if (cap == 0 || cap > 4096) cap = 256;

    memset(s.slots, 0, sizeof(s.slots));
    s.slot_count = 0;
    s.total_used = 0;

    uint32_t head = hdr->ring_head;
    bool seen[OOM_MAX_SLOTS] = {false};

    for (uint32_t i = 0; i < cap && s.slot_count < OOM_MAX_SLOTS; i++) {
        uint32_t idx = (head + cap - 1 - i) % cap;
        const MemSnapshot *snap = &ring[idx];
        uint8_t sid = snap->slot_id;
        if (sid >= OOM_MAX_SLOTS) continue;
        if (seen[sid]) continue;
        seen[sid] = true;

        OomSlotInfo *slot = &s.slots[s.slot_count++];
        slot->slot_id   = sid;
        slot->priority  = snap->priority;
        slot->heap_used = snap->heap_used;
        slot->age_ticks = snap->age_ticks;
        slot->active    = (snap->heap_used > 0);
        slot->pinned    = (snap->pinned != 0);
        s.total_used   += snap->heap_used;
    }
}

/* ── Eviction logic ───────────────────────────────────────────────── */

static void evict_one(bool hard) {
    if (!hard && (s.tick - s.last_evict_tick) < OOM_COOLDOWN_TICKS) {
        LOG_VMM("oom_killer: cooldown active, skipping eviction\n");
        return;
    }

    refresh_slot_table();

    int      victim_idx   = -1;
    uint64_t victim_score = UINT64_MAX;

    for (int i = 0; i < s.slot_count; i++) {
        if (!s.slots[i].active || s.slots[i].pinned) continue;
        uint64_t score = oom_score(&s.slots[i]);
        if (score < victim_score) {
            victim_score = score;
            victim_idx   = i;
        }
    }

    if (victim_idx < 0) {
        LOG_VMM("oom_killer: no evictable slot found\n");
        return;
    }

    uint8_t victim_slot = s.slots[victim_idx].slot_id;

    if (!hard && g_agentfs_ep != 0) {
#ifndef AGENTOS_TEST_HOST
        seL4_SetMR(0, OP_SNAPSHOT_SLOT);
        seL4_SetMR(1, victim_slot);
        seL4_MessageInfo_t _i = seL4_MessageInfo_new(OP_SNAPSHOT_SLOT, 0, 0, 2);
        (void)seL4_Call(g_agentfs_ep, _i);
#endif
    }

    if (g_gpu_sched_ep != 0) {
#ifndef AGENTOS_TEST_HOST
        seL4_SetMR(0, OP_SLOT_EVICT);
        seL4_SetMR(1, victim_slot);
        seL4_MessageInfo_t _i = seL4_MessageInfo_new(OP_SLOT_EVICT, 0, 0, 2);
        (void)seL4_Call(g_gpu_sched_ep, _i);
#endif
    }

    s.evict_count++;
    s.last_evicted_slot = victim_slot;
    s.last_evict_tick   = s.tick;
    LOG_VMM("oom_killer: evicted slot\n");
}

static void check_pressure(void) {
    refresh_slot_table();
    if (s.total_budget == 0) return;

    uint32_t pct = (uint32_t)((s.total_used * 100) / s.total_budget);

    if (pct >= s.hard_pct) {
        LOG_VMM("oom_killer: HARD pressure — emergency eviction\n");
        evict_one(true);
    } else if (pct >= s.threshold_pct || s.throttled) {
        LOG_VMM("oom_killer: soft pressure — evicting\n");
        evict_one(false);
    }
}

/* ── msg helpers ─────────────────────────────────────────────────── */

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

/* ── IPC handlers ─────────────────────────────────────────────────── */

static uint32_t h_status(sel4_badge_t b, const sel4_msg_t *req,
                          sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    s.tick++;
    check_pressure();
    rep_u32(rep, 0, 0);
    rep_u32(rep, 4, (uint32_t)(s.evict_count & 0xFFFFFFFF));
    rep_u32(rep, 8, s.last_evicted_slot);
    rep_u32(rep, 12, (uint32_t)(s.total_used & 0xFFFFFFFF));
    rep->length = 16;
    return SEL4_ERR_OK;
}

static uint32_t h_set_threshold(sel4_badge_t b, const sel4_msg_t *req,
                                  sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t pct = msg_u32(req, 4);
    if (pct > 0 && pct < 100) {
        s.threshold_pct = pct;
        LOG_VMM("oom_killer: threshold updated\n");
    }
    rep_u32(rep, 0, 0);
    rep->length = 4;
    return SEL4_ERR_OK;
}

void oom_killer_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    memset(&s, 0, sizeof(s));
    s.threshold_pct = OOM_THRESHOLD_PCT;
    s.hard_pct      = OOM_HARD_PCT;
    sel4_dbg_puts("[oom_killer] ready (threshold=80% hard=95%)\n");

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, OP_OOM_STATUS,        h_status,        (void *)0);
    sel4_server_register(&srv, OP_OOM_SET_THRESHOLD, h_set_threshold, (void *)0);
    sel4_server_run(&srv);
}
