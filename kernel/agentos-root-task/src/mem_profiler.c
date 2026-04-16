/*
 * agentOS MemProfiler Protection Domain
 *
 * Passive PD at priority 108. Tracks WASM heap allocations per agent slot
 * and alerts the watchdog (via controller) when a leak is detected.
 *
 * Design:
 *   - Workers call OP_MEM_ALLOC_HOOK / OP_MEM_FREE_HOOK via PPC
 *     whenever wasm_malloc / wasm_free are called in their sandbox.
 *   - MemProfiler maintains a per-slot alloc table (alloc count + bytes).
 *   - Every 30 notify ticks from the controller, it writes a snapshot
 *     of all slot stats into a 256KB shared-memory ring (mem_ring).
 *   - If any slot's live_alloc_count has grown monotonically for
 *     LEAK_THRESHOLD_TICKS (100) ticks, MemProfiler notifies the
 *     controller with OP_MEM_LEAK_ALERT so the watchdog can restart
 *     the offending slot.
 *
 * Channel assignments:
 *   CH_CONTROLLER = 0  (controller notifies for tick; receives leak alert)
 *   CH_WORKER_0..7 = 1..8  (workers PPC in for alloc/free hooks + status)
 *
 * Shared memory:
 *   mem_ring (256KB): ring buffer of per-slot heap snapshots
 *     - Written by mem_profiler every 30 ticks
 *     - Read by controller / external debugger
 *
 * IPC operations (MR0 = op code):
 *   OP_MEM_REGISTER   = 0x60 — register a slot (worker → mem_profiler)
 *   OP_MEM_ALLOC_HOOK = 0x61 — record a malloc(size) for slot (MR1=slot, MR2=size)
 *   OP_MEM_FREE_HOOK  = 0x62 — record a free(size) for slot  (MR1=slot, MR2=size)
 *   OP_MEM_STATUS     = 0x63 — query live stats for a slot   (MR1=slot)
 *   OP_MEM_SNAPSHOT   = 0x64 — force snapshot write to ring
 *
 * Controller out-of-band (notify, not PPC):
 *   CH_CONTROLLER notify → tick counter increment + snapshot at tick%30==0
 *
 * MemProfiler → controller PPC (leak alert):
 *   OP_MEM_LEAK_ALERT = 0x65 — MR1=slot_id, MR2=live_allocs, MR3=live_bytes
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "prio_inherit.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Channel IDs ────────────────────────────────────────────────────── */
#define CH_CONTROLLER   0   /* controller notifies for tick; mem_profiler PPCs for alert */
#define CH_WORKER_BASE  1   /* workers 0..7 occupy CH_WORKER_BASE + slot_id */

/* ── Op codes ───────────────────────────────────────────────────────── */
#define OP_MEM_REGISTER    0x60
#define OP_MEM_ALLOC_HOOK  0x61
#define OP_MEM_FREE_HOOK   0x62
#define OP_MEM_STATUS      0x63
#define OP_MEM_SNAPSHOT    0x64
#define OP_MEM_LEAK_ALERT  0x65

/* ── Result codes ───────────────────────────────────────────────────── */
#define MEM_OK             0
#define MEM_ERR_BADSLOT    1
#define MEM_ERR_UNDERFLOW  2
#define MEM_ERR_INTERNAL   99

/* ── Configuration ──────────────────────────────────────────────────── */
#define MAX_SLOTS              8
#define LEAK_THRESHOLD_TICKS  100   /* consecutive ticks of monotonic growth */
#define SNAPSHOT_INTERVAL      30   /* ticks between ring writes */

/* ── Shared memory ring ─────────────────────────────────────────────── */
/* Microkit setvar_vaddr: patched to the mapped virtual address */
uintptr_t mem_ring_vaddr;

#define MEM_RING_SIZE          0x40000UL   /* 256KB ring */
#define MEM_RING_RECORD_SIZE   64          /* bytes per snapshot record */
#define MEM_RING_CAPACITY      (MEM_RING_SIZE / MEM_RING_RECORD_SIZE)

/* Snapshot record layout (64 bytes, packed) */
typedef struct __attribute__((packed)) {
    uint64_t  tick;                /* system tick at snapshot time */
    uint8_t   slot_id;
    uint8_t   _pad[3];
    uint32_t  live_allocs;         /* current live allocation count */
    uint64_t  live_bytes;          /* current live allocation bytes */
    uint64_t  total_allocs;        /* lifetime allocation count */
    uint64_t  total_frees;         /* lifetime free count */
    uint64_t  total_alloc_bytes;   /* lifetime bytes allocated */
    uint64_t  total_free_bytes;    /* lifetime bytes freed */
    uint8_t   leak_suspected;      /* 1 if monotonic counter hit threshold */
    uint8_t   _pad2[7];
} MemSnapshot;   /* = 64 bytes */

_Static_assert(sizeof(MemSnapshot) == MEM_RING_RECORD_SIZE,
               "MemSnapshot must be exactly 64 bytes");

/* ── Per-slot tracking ──────────────────────────────────────────────── */
typedef struct {
    bool     registered;
    uint32_t live_allocs;          /* currently live (not yet freed) alloc count */
    uint64_t live_bytes;           /* currently live bytes */
    uint64_t total_allocs;
    uint64_t total_frees;
    uint64_t total_alloc_bytes;
    uint64_t total_free_bytes;
    uint32_t prev_live_allocs;     /* live_allocs at last tick snapshot */
    uint32_t monotonic_ticks;      /* ticks since live_allocs last decreased */
    bool     leak_alerted;         /* have we already fired the alert? */
} SlotState;

/* ── Global state ───────────────────────────────────────────────────── */
static struct {
    SlotState slots[MAX_SLOTS];
    uint64_t  tick;                /* total notify-tick counter */
    uint32_t  ring_head;           /* next write index into ring */
    bool      ring_ready;          /* ring memory is mapped */
} mpstate;

/* ── Helpers ────────────────────────────────────────────────────────── */

static void put_str(const char *s) {
    log_drain_write(8, 8, s);
}

static void put_dec(uint64_t v) {
    if (v == 0) { put_str("0"); return; }
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    while (v > 0 && i > 0) {
        buf[--i] = '0' + (int)(v % 10);
        v /= 10;
    }
    put_str(&buf[i]);
}

static bool valid_slot(uint32_t slot) {
    return slot < MAX_SLOTS;
}

/* Write a snapshot record into the ring for a given slot */
static void ring_write(uint8_t slot_id) {
    if (!mpstate.ring_ready) return;
    SlotState *s = &mpstate.slots[slot_id];

    MemSnapshot *ring = (MemSnapshot *)(uintptr_t)mem_ring_vaddr;
    uint32_t idx = mpstate.ring_head % MEM_RING_CAPACITY;
    MemSnapshot *rec = &ring[idx];

    rec->tick              = mpstate.tick;
    rec->slot_id           = slot_id;
    rec->live_allocs       = s->live_allocs;
    rec->live_bytes        = s->live_bytes;
    rec->total_allocs      = s->total_allocs;
    rec->total_frees       = s->total_frees;
    rec->total_alloc_bytes = s->total_alloc_bytes;
    rec->total_free_bytes  = s->total_free_bytes;
    rec->leak_suspected    = s->monotonic_ticks >= LEAK_THRESHOLD_TICKS ? 1 : 0;

    /* Zero padding */
    rec->_pad[0] = rec->_pad[1] = rec->_pad[2] = 0;
    rec->_pad2[0] = rec->_pad2[1] = rec->_pad2[2] = rec->_pad2[3] = 0;
    rec->_pad2[4] = rec->_pad2[5] = rec->_pad2[6] = rec->_pad2[7] = 0;

    mpstate.ring_head++;
}

/* Called every SNAPSHOT_INTERVAL ticks: write all registered slots */
static void snapshot_all(void) {
    for (uint8_t i = 0; i < MAX_SLOTS; i++) {
        if (mpstate.slots[i].registered) {
            ring_write(i);
        }
    }
    put_str("[mem_profiler] snapshot tick=");
    put_dec(mpstate.tick);
    put_str("\n");
}

/* Leak detection: called each tick per registered slot */
static void check_leak(uint8_t slot_id) {
    SlotState *s = &mpstate.slots[slot_id];
    if (!s->registered || s->leak_alerted) return;

    if (s->live_allocs > s->prev_live_allocs) {
        s->monotonic_ticks++;
    } else {
        s->monotonic_ticks = 0;
    }
    s->prev_live_allocs = s->live_allocs;

    if (s->monotonic_ticks >= LEAK_THRESHOLD_TICKS) {
        /* Fire leak alert to controller */
        put_str("[mem_profiler] LEAK DETECTED slot=");
        put_dec(slot_id);
        put_str(" live_allocs=");
        put_dec(s->live_allocs);
        put_str(" live_bytes=");
        put_dec(s->live_bytes);
        put_str("\n");

        /* PPC into controller with OP_MEM_LEAK_ALERT */
        microkit_mr_set(0, OP_MEM_LEAK_ALERT);
        microkit_mr_set(1, slot_id);
        microkit_mr_set(2, s->live_allocs);
        microkit_mr_set(3, (uint32_t)(s->live_bytes >> 32));
        microkit_mr_set(4, (uint32_t)(s->live_bytes & 0xFFFFFFFF));
        /* mem_profiler (prio 108) PPC-ing controller (prio 50) — no inversion possible
         * (we're higher prio), but use PPCALL_DONATE for uniform API consistency. */
        PPCALL_DONATE(CH_CONTROLLER, microkit_msginfo_new(0, 5),
                      PRIO_MEM_PROFILER, PRIO_CONTROLLER);

        s->leak_alerted = true;  /* suppress repeat alerts until reset */
    }
}

/* ── Microkit entry points ──────────────────────────────────────────── */

void init(void) {
    put_str("[mem_profiler] init — priority 108, passive\n");

    /* Zero all state */
    for (int i = 0; i < MAX_SLOTS; i++) {
        mpstate.slots[i] = (SlotState){0};
    }
    mpstate.tick       = 0;
    mpstate.ring_head  = 0;
    mpstate.ring_ready = (mem_ring_vaddr != 0);

    if (mpstate.ring_ready) {
        put_str("[mem_profiler] ring mapped, 256KB, capacity=");
        put_dec(MEM_RING_CAPACITY);
        put_str(" records\n");
    } else {
        put_str("[mem_profiler] WARNING: ring not mapped\n");
    }
}

/*
 * notified() — controller sends a tick notification (CH_CONTROLLER).
 */
void notified(microkit_channel ch) {
    (void)ch;
    mpstate.tick++;

    /* Leak detection: check all registered slots */
    for (uint8_t i = 0; i < MAX_SLOTS; i++) {
        if (mpstate.slots[i].registered) {
            check_leak(i);
        }
    }

    /* Periodic snapshot */
    if (mpstate.tick % SNAPSHOT_INTERVAL == 0) {
        snapshot_all();
    }
}

/*
 * protected() — workers PPC in for alloc/free hooks and status queries.
 * Returns microkit_msginfo with MR0 = result code.
 */
microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)ch;
    (void)msginfo;

    uint32_t op   = (uint32_t)microkit_mr_get(0);
    uint32_t slot = (uint32_t)microkit_mr_get(1);

    switch (op) {

    /* ── OP_MEM_REGISTER ──────────────────────────────────────────── */
    case OP_MEM_REGISTER: {
        if (!valid_slot(slot)) {
            microkit_mr_set(0, MEM_ERR_BADSLOT);
            return microkit_msginfo_new(0, 1);
        }
        SlotState *s = &mpstate.slots[slot];
        s->registered       = true;
        s->live_allocs      = 0;
        s->live_bytes       = 0;
        s->total_allocs     = 0;
        s->total_frees      = 0;
        s->total_alloc_bytes = 0;
        s->total_free_bytes  = 0;
        s->prev_live_allocs = 0;
        s->monotonic_ticks  = 0;
        s->leak_alerted     = false;
        put_str("[mem_profiler] slot=");
        put_dec(slot);
        put_str(" registered\n");
        microkit_mr_set(0, MEM_OK);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_MEM_ALLOC_HOOK ────────────────────────────────────────── */
    case OP_MEM_ALLOC_HOOK: {
        if (!valid_slot(slot)) {
            microkit_mr_set(0, MEM_ERR_BADSLOT);
            return microkit_msginfo_new(0, 1);
        }
        uint32_t size = (uint32_t)microkit_mr_get(2);
        SlotState *s = &mpstate.slots[slot];
        s->live_allocs++;
        s->live_bytes       += size;
        s->total_allocs++;
        s->total_alloc_bytes += size;
        microkit_mr_set(0, MEM_OK);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_MEM_FREE_HOOK ─────────────────────────────────────────── */
    case OP_MEM_FREE_HOOK: {
        if (!valid_slot(slot)) {
            microkit_mr_set(0, MEM_ERR_BADSLOT);
            return microkit_msginfo_new(0, 1);
        }
        uint32_t size = (uint32_t)microkit_mr_get(2);
        SlotState *s = &mpstate.slots[slot];
        if (s->live_allocs == 0) {
            /* Guard against underflow from mismatched free */
            microkit_mr_set(0, MEM_ERR_UNDERFLOW);
            return microkit_msginfo_new(0, 1);
        }
        s->live_allocs--;
        if (s->live_bytes >= size) s->live_bytes -= size;
        else s->live_bytes = 0;  /* clamp on underflow */
        s->total_frees++;
        s->total_free_bytes += size;
        /* Reset leak alert if pressure subsides */
        if (s->live_allocs < s->prev_live_allocs) {
            s->leak_alerted = false;
        }
        microkit_mr_set(0, MEM_OK);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_MEM_STATUS ────────────────────────────────────────────── */
    case OP_MEM_STATUS: {
        if (!valid_slot(slot)) {
            microkit_mr_set(0, MEM_ERR_BADSLOT);
            return microkit_msginfo_new(0, 1);
        }
        SlotState *s = &mpstate.slots[slot];
        microkit_mr_set(0, MEM_OK);
        microkit_mr_set(1, s->live_allocs);
        microkit_mr_set(2, (uint32_t)(s->live_bytes >> 32));
        microkit_mr_set(3, (uint32_t)(s->live_bytes & 0xFFFFFFFF));
        microkit_mr_set(4, (uint32_t)(s->total_allocs & 0xFFFFFFFF));
        microkit_mr_set(5, (uint32_t)(s->total_frees  & 0xFFFFFFFF));
        microkit_mr_set(6, s->monotonic_ticks);
        microkit_mr_set(7, s->leak_alerted ? 1 : 0);
        return microkit_msginfo_new(0, 8);
    }

    /* ── OP_MEM_SNAPSHOT ──────────────────────────────────────────── */
    case OP_MEM_SNAPSHOT: {
        snapshot_all();
        microkit_mr_set(0, MEM_OK);
        microkit_mr_set(1, (uint32_t)(mpstate.ring_head & 0xFFFFFFFF));
        return microkit_msginfo_new(0, 2);
    }

    default:
        microkit_mr_set(0, MEM_ERR_INTERNAL);
        return microkit_msginfo_new(0, 1);
    }
}
