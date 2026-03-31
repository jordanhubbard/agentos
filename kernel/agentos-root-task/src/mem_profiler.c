/*
 * agentOS WASM Memory Profiler Protection Domain
 *
 * Passive PD (priority 108) that tracks per-slot WASM heap allocations and
 * detects memory leaks via a monotonic-growth heuristic.
 *
 * Design:
 *   - Shared-memory hook table (256KB MR "mem_profiler_ring") stores per-slot
 *     alloc counts, byte totals, and a snapshot log.
 *   - init_agent calls OP_MEM_REGISTER when it spawns a WASM slot.
 *   - The WASM host (wasm3_host.c) calls OP_MEM_ALLOC / OP_MEM_FREE on every
 *     wasm_malloc / wasm_free intercept.
 *   - Every 30 ticks mem_profiler writes a heap snapshot entry to AgentFS.
 *   - When a slot's live_allocs grows monotonically for 100 consecutive ticks,
 *     mem_profiler sends MSG_MEM_LEAK_ALERT to the watchdog channel.
 *
 * IPC operations:
 *   OP_MEM_REGISTER  (0xC0) — register a new WASM slot
 *   OP_MEM_ALLOC     (0xC1) — record allocation: slot_id, ptr, size
 *   OP_MEM_FREE      (0xC2) — record free: slot_id, ptr, size
 *   OP_MEM_STATUS    (0xC3) — query live alloc count + bytes for a slot
 *   OP_MEM_SNAPSHOT  (0xC4) — force an immediate snapshot for a slot
 *
 * Channels (from mem_profiler's perspective):
 *   id=0: controller PPCs in  (register/status/snapshot)
 *   id=1: init_agent PPCs in  (register on spawn)
 *   id=2: wasm_host PPCs in   (alloc/free)
 *   id=3: mem_profiler -> watchdog (notify: MSG_MEM_LEAK_ALERT on leak)
 *
 * Memory:
 *   mem_profiler_ring (256KB shared MR): slot table + snapshot log
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"

/* ── Opcodes ──────────────────────────────────────────────────────────────── */
#define OP_MEM_REGISTER  0xC0  /* Register: MR1=slot_id */
#define OP_MEM_ALLOC     0xC1  /* Alloc:    MR1=slot_id, MR2=ptr_lo, MR3=ptr_hi, MR4=size */
#define OP_MEM_FREE      0xC2  /* Free:     MR1=slot_id, MR2=ptr_lo, MR3=ptr_hi, MR4=size */
#define OP_MEM_STATUS    0xC3  /* Status:   MR1=slot_id → MR0=live_allocs, MR1=live_bytes, MR2=total_allocs, MR3=flags */
#define OP_MEM_SNAPSHOT  0xC4  /* Snapshot: MR1=slot_id (force immediate log write) */

/* Alert message type for watchdog channel */
#define MSG_MEM_LEAK_ALERT  0x0C01  /* mem_profiler -> watchdog: slot_id has a leak */

/* Leak detection: flag a leak when live_allocs grows monotonically this many ticks */
#define LEAK_MONOTONE_THRESHOLD  100
/* Snapshot interval: write AgentFS snapshot every N ticks */
#define SNAPSHOT_INTERVAL        30

/* ── Slot flags ───────────────────────────────────────────────────────────── */
#define MEM_FLAG_ACTIVE       (1u << 0)
#define MEM_FLAG_LEAK_FLAGGED (1u << 1)  /* leak alert already sent */

/* ── Per-slot tracking entry ──────────────────────────────────────────────── */
#define MAX_MEM_SLOTS  16

typedef struct {
    uint32_t slot_id;          /* WASM agent slot identifier */
    uint32_t flags;            /* MEM_FLAG_* */
    uint64_t live_allocs;      /* current live allocation count */
    uint64_t live_bytes;       /* current live allocated bytes */
    uint64_t total_allocs;     /* cumulative alloc calls since register */
    uint64_t total_frees;      /* cumulative free calls since register */
    uint64_t total_alloc_bytes;/* cumulative bytes allocated */
    uint64_t total_free_bytes; /* cumulative bytes freed */
    uint64_t prev_live_allocs; /* live_allocs at start of last monotone window */
    uint32_t monotone_ticks;   /* consecutive ticks where live_allocs increased */
    uint32_t snapshot_tick;    /* tick of last snapshot */
} mem_slot_t;

/* ── Snapshot log entry (48 bytes) ───────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t seq;              /* monotonic sequence */
    uint64_t tick;             /* boot tick */
    uint32_t slot_id;
    uint32_t flags;            /* slot flags at snapshot time */
    uint64_t live_allocs;
    uint64_t live_bytes;
    uint64_t total_allocs;
    uint64_t total_frees;
} mem_snapshot_t;

/* ── Shared memory layout (256KB = 0x40000) ──────────────────────────────── */
/*
 *   [0..63]                   — header (64 bytes)
 *   [64..64+16*72-1]          — slot table (16 slots × 72 bytes = 1152 bytes)
 *   [1216..]                  — snapshot log ring (remaining / 48 bytes per entry)
 */

typedef struct __attribute__((packed)) {
    uint32_t magic;            /* 0x0CAFE01D ("mem profiler online dad") */
    uint32_t version;          /* 1 */
    uint32_t slot_count;       /* MAX_MEM_SLOTS */
    uint32_t active_count;
    uint64_t snap_head;        /* next write index in snapshot log */
    uint64_t snap_count;       /* total snapshots written */
    uint64_t snap_capacity;    /* number of snapshot slots */
    uint64_t boot_tick;        /* current boot tick */
    uint8_t  _pad[16];         /* pad to 64 bytes */
} mem_profiler_header_t;

#define MEM_PROFILER_MAGIC  0x0CAFE01D

uintptr_t mem_profiler_ring_vaddr;

#define MP_HDR       ((volatile mem_profiler_header_t *)mem_profiler_ring_vaddr)
#define MP_TABLE     ((volatile mem_slot_t *) \
    ((uint8_t *)mem_profiler_ring_vaddr + sizeof(mem_profiler_header_t)))
#define MP_SNAPSHOTS ((volatile mem_snapshot_t *) \
    ((uint8_t *)mem_profiler_ring_vaddr + sizeof(mem_profiler_header_t) + \
     MAX_MEM_SLOTS * sizeof(mem_slot_t)))

/* Channel IDs from mem_profiler's perspective */
#define MP_CH_CONTROLLER  0
#define MP_CH_INIT_AGENT  1
#define MP_CH_WASM_HOST   2
#define MP_CH_WATCHDOG    3

static uint64_t boot_tick = 0;

/* ── Helper: decimal print ────────────────────────────────────────────────── */
static void put_dec(uint32_t v) {
    if (v == 0) { microkit_dbg_puts("0"); return; }
    char buf[12]; int i = 11;
    buf[i] = '\0';
    while (v > 0 && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; }
    microkit_dbg_puts(&buf[i]);
}

/* ── Init ─────────────────────────────────────────────────────────────────── */
static void mem_profiler_init(void) {
    volatile mem_profiler_header_t *hdr = MP_HDR;

    uint64_t region_size  = 0x40000; /* 256KB */
    uint64_t table_size   = sizeof(mem_profiler_header_t)
                          + MAX_MEM_SLOTS * sizeof(mem_slot_t);
    uint64_t snap_space   = region_size - table_size;
    uint64_t snap_cap     = snap_space / sizeof(mem_snapshot_t);

    hdr->magic          = MEM_PROFILER_MAGIC;
    hdr->version        = 1;
    hdr->slot_count     = MAX_MEM_SLOTS;
    hdr->active_count   = 0;
    hdr->snap_head      = 0;
    hdr->snap_count     = 0;
    hdr->snap_capacity  = snap_cap;
    hdr->boot_tick      = 0;

    volatile mem_slot_t *table = MP_TABLE;
    for (int i = 0; i < MAX_MEM_SLOTS; i++) {
        table[i].slot_id           = 0;
        table[i].flags             = 0;
        table[i].live_allocs       = 0;
        table[i].live_bytes        = 0;
        table[i].total_allocs      = 0;
        table[i].total_frees       = 0;
        table[i].total_alloc_bytes = 0;
        table[i].total_free_bytes  = 0;
        table[i].prev_live_allocs  = 0;
        table[i].monotone_ticks    = 0;
        table[i].snapshot_tick     = 0;
    }

    microkit_dbg_puts("[mem_profiler] Initialized: ");
    put_dec(MAX_MEM_SLOTS);
    microkit_dbg_puts(" slots, ");
    put_dec((uint32_t)snap_cap);
    microkit_dbg_puts(" snapshot entries\n");
}

/* ── Write snapshot ───────────────────────────────────────────────────────── */
static void write_snapshot(volatile mem_slot_t *s) {
    volatile mem_profiler_header_t *hdr = MP_HDR;
    volatile mem_snapshot_t *snaps = MP_SNAPSHOTS;

    if (hdr->snap_capacity == 0) return;
    uint64_t idx = hdr->snap_head % hdr->snap_capacity;
    volatile mem_snapshot_t *e = &snaps[idx];

    e->seq          = hdr->snap_count;
    e->tick         = boot_tick;
    e->slot_id      = s->slot_id;
    e->flags        = s->flags;
    e->live_allocs  = s->live_allocs;
    e->live_bytes   = s->live_bytes;
    e->total_allocs = s->total_allocs;
    e->total_frees  = s->total_frees;

    hdr->snap_head = (hdr->snap_head + 1) % hdr->snap_capacity;
    hdr->snap_count++;
    s->snapshot_tick = (uint32_t)boot_tick;

    microkit_dbg_puts("[mem_profiler] snapshot slot=");
    put_dec(s->slot_id);
    microkit_dbg_puts(" live_allocs=");
    put_dec((uint32_t)s->live_allocs);
    microkit_dbg_puts(" live_bytes=");
    put_dec((uint32_t)s->live_bytes);
    microkit_dbg_puts("\n");
}

/* ── Send leak alert to watchdog ──────────────────────────────────────────── */
static void alert_watchdog(uint32_t slot_id) {
    microkit_mr_set(0, MSG_MEM_LEAK_ALERT);
    microkit_mr_set(1, slot_id);
    microkit_mr_set(2, (uint32_t)boot_tick);
    microkit_notify(MP_CH_WATCHDOG);

    microkit_dbg_puts("[mem_profiler] LEAK ALERT slot=");
    put_dec(slot_id);
    microkit_dbg_puts(" monotone_ticks=");
    put_dec(LEAK_MONOTONE_THRESHOLD);
    microkit_dbg_puts("\n");
}

/* ── Per-tick work: snapshot + leak check ─────────────────────────────────── */
static void tick_all_slots(void) {
    volatile mem_slot_t *table = MP_TABLE;

    for (int i = 0; i < MAX_MEM_SLOTS; i++) {
        volatile mem_slot_t *s = &table[i];
        if (!(s->flags & MEM_FLAG_ACTIVE)) continue;

        /* Leak detection: check monotonic growth */
        if (s->live_allocs > s->prev_live_allocs) {
            s->monotone_ticks++;
            if (s->monotone_ticks >= LEAK_MONOTONE_THRESHOLD &&
                !(s->flags & MEM_FLAG_LEAK_FLAGGED)) {
                s->flags |= MEM_FLAG_LEAK_FLAGGED;
                alert_watchdog(s->slot_id);
            }
        } else {
            s->monotone_ticks = 0;
        }
        s->prev_live_allocs = s->live_allocs;

        /* Snapshot every SNAPSHOT_INTERVAL ticks */
        uint32_t ticks_since = (uint32_t)(boot_tick - s->snapshot_tick);
        if (ticks_since >= SNAPSHOT_INTERVAL) {
            write_snapshot(s);
        }
    }
}

/* ── Slot lookup helpers ──────────────────────────────────────────────────── */
static int find_slot(uint32_t slot_id) {
    volatile mem_slot_t *table = MP_TABLE;
    for (int i = 0; i < MAX_MEM_SLOTS; i++) {
        if ((table[i].flags & MEM_FLAG_ACTIVE) && table[i].slot_id == slot_id)
            return i;
    }
    return -1;
}

static int find_free_slot(void) {
    volatile mem_slot_t *table = MP_TABLE;
    for (int i = 0; i < MAX_MEM_SLOTS; i++) {
        if (!(table[i].flags & MEM_FLAG_ACTIVE)) return i;
    }
    return -1;
}

/* ── Handle PPC requests ──────────────────────────────────────────────────── */
static microkit_msginfo handle_request(microkit_msginfo msg) {
    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {

    case OP_MEM_REGISTER: {
        uint32_t slot_id = (uint32_t)microkit_mr_get(1);

        if (find_slot(slot_id) >= 0) {
            microkit_dbg_puts("[mem_profiler] WARN: slot already registered\n");
            microkit_mr_set(0, 1);  /* already exists */
            return microkit_msginfo_new(0, 1);
        }

        int idx = find_free_slot();
        if (idx < 0) {
            microkit_dbg_puts("[mem_profiler] ERROR: slot table full\n");
            microkit_mr_set(0, 0xE1);  /* ERR_TABLE_FULL */
            return microkit_msginfo_new(0, 1);
        }

        volatile mem_slot_t *s = &MP_TABLE[idx];
        s->slot_id           = slot_id;
        s->flags             = MEM_FLAG_ACTIVE;
        s->live_allocs       = 0;
        s->live_bytes        = 0;
        s->total_allocs      = 0;
        s->total_frees       = 0;
        s->total_alloc_bytes = 0;
        s->total_free_bytes  = 0;
        s->prev_live_allocs  = 0;
        s->monotone_ticks    = 0;
        s->snapshot_tick     = (uint32_t)boot_tick;

        MP_HDR->active_count++;

        microkit_dbg_puts("[mem_profiler] Registered slot=");
        put_dec(slot_id);
        microkit_dbg_puts(" idx=");
        put_dec((uint32_t)idx);
        microkit_dbg_puts("\n");

        microkit_mr_set(0, 0);  /* success */
        microkit_mr_set(1, (uint32_t)idx);
        return microkit_msginfo_new(0, 2);
    }

    case OP_MEM_ALLOC: {
        uint32_t slot_id = (uint32_t)microkit_mr_get(1);
        /* MR2=ptr_lo, MR3=ptr_hi — logged but not used for tracking */
        uint32_t size    = (uint32_t)microkit_mr_get(4);

        int idx = find_slot(slot_id);
        if (idx < 0) {
            microkit_mr_set(0, 0xE2);  /* ERR_NOT_FOUND */
            return microkit_msginfo_new(0, 1);
        }

        volatile mem_slot_t *s = &MP_TABLE[idx];
        s->live_allocs++;
        s->live_bytes        += size;
        s->total_allocs++;
        s->total_alloc_bytes += size;

        /* Clear leak flag if allocs are being actively freed (healthy pattern) */
        if (s->live_allocs == 0) s->flags &= ~MEM_FLAG_LEAK_FLAGGED;

        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    case OP_MEM_FREE: {
        uint32_t slot_id = (uint32_t)microkit_mr_get(1);
        uint32_t size    = (uint32_t)microkit_mr_get(4);

        int idx = find_slot(slot_id);
        if (idx < 0) {
            microkit_mr_set(0, 0xE2);
            return microkit_msginfo_new(0, 1);
        }

        volatile mem_slot_t *s = &MP_TABLE[idx];
        if (s->live_allocs > 0) s->live_allocs--;
        if (s->live_bytes >= size) s->live_bytes -= size;
        else                       s->live_bytes  = 0;
        s->total_frees++;
        s->total_free_bytes += size;

        /* Healthy free activity resets leak window */
        if (s->live_allocs < s->prev_live_allocs) {
            s->monotone_ticks = 0;
        }

        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    case OP_MEM_STATUS: {
        uint32_t slot_id = (uint32_t)microkit_mr_get(1);

        int idx = find_slot(slot_id);
        if (idx < 0) {
            microkit_mr_set(0, 0xE2);
            return microkit_msginfo_new(0, 1);
        }

        volatile mem_slot_t *s = &MP_TABLE[idx];
        microkit_mr_set(0, (uint32_t)s->live_allocs);
        microkit_mr_set(1, (uint32_t)s->live_bytes);
        microkit_mr_set(2, (uint32_t)s->total_allocs);
        microkit_mr_set(3, s->flags);
        microkit_mr_set(4, s->monotone_ticks);
        return microkit_msginfo_new(0, 5);
    }

    case OP_MEM_SNAPSHOT: {
        uint32_t slot_id = (uint32_t)microkit_mr_get(1);

        int idx = find_slot(slot_id);
        if (idx < 0) {
            microkit_mr_set(0, 0xE2);
            return microkit_msginfo_new(0, 1);
        }

        write_snapshot(&MP_TABLE[idx]);
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    default:
        microkit_dbg_puts("[mem_profiler] WARN: unknown opcode\n");
        microkit_mr_set(0, 0xFF);
        return microkit_msginfo_new(0, 1);
    }

    (void)msg;
}

/* ── Microkit entry points ────────────────────────────────────────────────── */
void init(void) {
    agentos_log_boot("mem_profiler");
    mem_profiler_init();
    microkit_dbg_puts("[mem_profiler] Ready — priority 108, passive, ");
    put_dec(MAX_MEM_SLOTS);
    microkit_dbg_puts(" WASM slot tracking slots\n");
}

microkit_msginfo protected(microkit_channel channel, microkit_msginfo msg) {
    (void)channel;
    boot_tick++;
    MP_HDR->boot_tick = boot_tick;
    tick_all_slots();
    return handle_request(msg);
}

void notified(microkit_channel ch) {
    (void)ch;
    boot_tick++;
    MP_HDR->boot_tick = boot_tick;
    tick_all_slots();
}
