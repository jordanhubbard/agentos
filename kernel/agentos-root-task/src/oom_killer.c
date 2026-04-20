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
 *   Incoming notifications:
 *     OOM_CH_MEM_PRESSURE (0) — mem_profiler alerts on threshold breach
 *     OOM_CH_POWER_THROTTLE (1) — power_mgr sends thermal alerts
 *
 *   Outgoing PPCs:
 *     OOM_CH_GPU_SCHED (2) → controller: OP_SLOT_EVICT (MR0=op, MR1=slot_id)
 *     OOM_CH_AGENTFS (3)   → AgentFS: OP_SNAPSHOT_SLOT (MR0=op, MR1=slot_id)
 *
 * Thresholds:
 *   OOM_THRESHOLD_PCT = 80  — trigger eviction at 80% heap budget
 *   OOM_HARD_PCT      = 95  — hard eviction (no snapshot) at 95%
 *
 * The total heap budget is derived from mem_profiler's shared ring header
 * (total_budget_bytes). On sparky GB10 with 192GB unified VRAM this is
 * approximately 192 × 1024³ bytes.
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/oom_killer_contract.h"
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

/* ── IPC channel IDs ──────────────────────────────────────────────── */

#define OOM_CH_MEM_PRESSURE   0
#define OOM_CH_POWER_THROTTLE 1
#define OOM_CH_GPU_SCHED      2
#define OOM_CH_AGENTFS        3

#ifdef AGENTOS_SNAPSHOT_SCHED
/* Channel to snapshot_sched — force a full KV snapshot before eviction. */
#define OOM_CH_SNAPSHOT_SCHED 4
/* OP_SS_FORCE_SNAPSHOT (0xA3) from snapshot_sched.c */
#define OP_SS_FORCE_SNAPSHOT  0xA3
#endif

/* ── Op codes ─────────────────────────────────────────────────────── */

#define OP_SLOT_EVICT         0xE0   /* MR0=op, MR1=slot_id */
#define OP_SNAPSHOT_SLOT      0xE1   /* MR0=op, MR1=slot_id */
#define OP_OOM_STATUS         0xE2   /* Query: MR0=op → MR1=evict_count, MR2=last_slot */
#define OP_OOM_SET_THRESHOLD  0xE3   /* Set threshold: MR1=pct (1-99) */
#define OP_MEM_PRESSURE_QUERY 0x64   /* mem_profiler OP_MEM_SNAPSHOT */

/* ── Thresholds ───────────────────────────────────────────────────── */

#define OOM_THRESHOLD_PCT     80     /* soft eviction trigger */
#define OOM_HARD_PCT          95     /* hard eviction, no snapshot */
#define OOM_COOLDOWN_TICKS    100    /* ticks between consecutive evictions */
#define OOM_MAX_SLOTS         16     /* maximum tracked slots */

/* ── Slot table (populated from mem_profiler shared ring) ─────────── */

typedef struct {
    uint8_t  slot_id;
    uint8_t  priority;       /* 0=lowest priority = evict first */
    uint64_t heap_used;      /* bytes currently allocated */
    uint64_t age_ticks;      /* ticks since last activity */
    bool     active;
    bool     pinned;         /* pinned slots are never evicted */
} OomSlotInfo;

/* ── Module state ─────────────────────────────────────────────────── */

typedef struct {
    OomSlotInfo  slots[OOM_MAX_SLOTS];
    int          slot_count;

    uint64_t     total_budget;      /* total heap budget in bytes */
    uint64_t     total_used;        /* sum of heap_used across active slots */
    uint32_t     threshold_pct;     /* soft threshold (default 80) */
    uint32_t     hard_pct;          /* hard threshold (default 95) */

    uint64_t     evict_count;       /* total evictions performed */
    uint8_t      last_evicted_slot; /* slot_id of last eviction */
    uint64_t     last_evict_tick;   /* tick counter of last eviction */

    bool         throttled;         /* true if power_mgr sent thermal alert */
    uint64_t     tick;              /* current tick */
} OomState;

static OomState s = {0};

/* ── mem_profiler shared ring MR ─────────────────────────────────── */

uintptr_t mem_ring_vaddr;   /* set by Microkit linker */

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
    uint64_t total_budget;   /* total system heap budget */
} MemRingHeader;

/* ── Scoring function ─────────────────────────────────────────────── */

static uint64_t oom_score(const OomSlotInfo *slot) {
    if (!slot->active || slot->pinned) return UINT64_MAX; /* never evict */
    if (slot->heap_used == 0) return 0;
    /* score = (heap × age) / (priority + 1) */
    uint64_t numerator = slot->heap_used * (slot->age_ticks + 1);
    uint64_t denom     = (uint64_t)(slot->priority + 1);
    return numerator / denom;
}

/* ── Read slot data from mem_profiler ring ────────────────────────── */

static void refresh_slot_table(void) {
    if (!mem_ring_vaddr) return;

    const MemRingHeader *hdr = (const MemRingHeader *)mem_ring_vaddr;
    if (hdr->magic != 0x4D454D52UL) return; /* "MEMR" */

    s.total_budget = hdr->total_budget ? hdr->total_budget : (192ULL * 1024 * 1024 * 1024);

    const MemSnapshot *ring = (const MemSnapshot *)(
        (uint8_t *)mem_ring_vaddr + sizeof(MemRingHeader));
    uint32_t cap = hdr->capacity;
    if (cap == 0 || cap > 4096) cap = 256;

    /* Zero out slot table */
    memset(s.slots, 0, sizeof(s.slots));
    s.slot_count  = 0;
    s.total_used  = 0;

    /* Walk ring entries from most recent */
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
    /* Cooldown guard */
    if (!hard && (s.tick - s.last_evict_tick) < OOM_COOLDOWN_TICKS) {
        LOG_VMM("oom_killer: cooldown active, skipping eviction\n");
        return;
    }

    refresh_slot_table();

    /* Find lowest-score evictable slot */
    int      victim_idx  = -1;
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
    LOG_VMM("oom_killer: evicting slot=%d heap=%llu priority=%d hard=%d\n",
            (int)victim_slot,
            (unsigned long long)s.slots[victim_idx].heap_used,
            (int)s.slots[victim_idx].priority,
            (int)hard);

    if (!hard) {
#ifdef AGENTOS_SNAPSHOT_SCHED
        /*
         * KV-level snapshot via snapshot_sched before eviction.
         * snapshot_sched writes the full KV store to AgentFS so the slot
         * can be restored after the eviction pressure subsides.
         */
        microkit_mr_set(0, OP_SS_FORCE_SNAPSHOT);
        microkit_ppcall(OOM_CH_SNAPSHOT_SCHED, microkit_msginfo_new(0, 1));
#endif
        /* Block-level snapshot to AgentFS (legacy — complements KV snapshot) */
        microkit_mr_set(0, OP_SNAPSHOT_SLOT);
        microkit_mr_set(1, victim_slot);
        microkit_ppcall(OOM_CH_AGENTFS, microkit_msginfo_new(0, 2));
    }

    /* Send eviction command to GPU scheduler via controller */
    microkit_mr_set(0, OP_SLOT_EVICT);
    microkit_mr_set(1, victim_slot);
    microkit_ppcall(OOM_CH_GPU_SCHED, microkit_msginfo_new(0, 2));

    s.evict_count++;
    s.last_evicted_slot = victim_slot;
    s.last_evict_tick   = s.tick;
}

static void check_pressure(void) {
    refresh_slot_table();

    if (s.total_budget == 0) return;

    uint32_t pct = (uint32_t)((s.total_used * 100) / s.total_budget);

    if (pct >= s.hard_pct) {
        LOG_VMM("oom_killer: HARD pressure %u%% >= %u%% — emergency eviction\n",
                pct, s.hard_pct);
        evict_one(true);
    } else if (pct >= s.threshold_pct || s.throttled) {
        LOG_VMM("oom_killer: soft pressure %u%% >= %u%% — evicting\n",
                pct, s.threshold_pct);
        evict_one(false);
    }
}

/* ── Microkit entry points ────────────────────────────────────────── */

void init(void) {
    memset(&s, 0, sizeof(s));
    s.threshold_pct = OOM_THRESHOLD_PCT;
    s.hard_pct      = OOM_HARD_PCT;
    LOG_VMM("oom_killer: ready (threshold=%u%% hard=%u%%)\n",
            s.threshold_pct, s.hard_pct);
}

void notified(microkit_channel ch) {
    s.tick++;
    switch (ch) {
        case OOM_CH_MEM_PRESSURE:
            LOG_VMM("oom_killer: mem_profiler pressure alert\n");
            check_pressure();
            break;
        case OOM_CH_POWER_THROTTLE:
            LOG_VMM("oom_killer: power_mgr thermal alert — lowering threshold\n");
            s.throttled = true;
            /* Immediately reduce pressure: evict even if below threshold */
            evict_one(false);
            break;
        default:
            LOG_VMM("oom_killer: unexpected notify ch=%d\n", (int)ch);
            break;
    }
}

seL4_MessageInfo_t protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)ch;
    uint64_t op = microkit_mr_get(0);
    switch (op) {
        case OP_OOM_STATUS:
            microkit_mr_set(0, 0); /* ok */
            microkit_mr_set(1, s.evict_count);
            microkit_mr_set(2, s.last_evicted_slot);
            microkit_mr_set(3, s.total_used);
            return microkit_msginfo_new(0, 4);

        case OP_OOM_SET_THRESHOLD: {
            uint64_t pct = microkit_mr_get(1);
            if (pct > 0 && pct < 100) {
                s.threshold_pct = (uint32_t)pct;
                LOG_VMM("oom_killer: threshold updated to %u%%\n", s.threshold_pct);
            }
            microkit_mr_set(0, 0);
            return microkit_msginfo_new(0, 1);
        }

        default:
            microkit_mr_set(0, 1); /* error */
            return microkit_msginfo_new(0, 1);
    }
    (void)msginfo;
}
