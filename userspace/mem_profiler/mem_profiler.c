/*
 * agentOS mem_profiler — per-slot WASM heap tracking + leak heuristic
 *
 * Demo flow (4 steps):
 *   1. REGISTER: init_agent calls OP_MP_ALLOC(slot_id=N, bytes=0) to register
 *      a new WASM slot; mem_profiler auto-allocates a tracking entry on first
 *      call for an unknown slot_id.
 *   2. ALLOC/FREE: wasm_host intercepts wasm_malloc/wasm_free and calls
 *      OP_MP_ALLOC(slot_id, bytes) / OP_MP_FREE(slot_id, bytes) on each event;
 *      live_alloc_count and live_bytes are updated, tick_monotonic incremented.
 *   3. SNAPSHOT: controller (or any caller) sends OP_MP_SNAPSHOT(slot_id) to
 *      read live_alloc_count (MR0) + live_bytes (MR1) for the slot; updates
 *      last_snapshot_bytes baseline and writes Prometheus-style text to the
 *      256KB mem_profiler_ring shared-memory region.
 *   4. LEAK ALERT: when 100 consecutive OP_MP_ALLOC ticks find live_bytes >
 *      last_snapshot_bytes, mem_profiler notifies watchdog via CH_OUT (id=1).
 *
 * Passive PD, priority 108.
 *
 * Opcodes (MR0):
 *   0x80 OP_MP_ALLOC    — MR0=opcode, MR1=slot_id, MR2=bytes: record alloc
 *   0x81 OP_MP_FREE     — MR0=opcode, MR1=slot_id, MR2=bytes: record free
 *   0x82 OP_MP_SNAPSHOT — MR0=opcode, MR1=slot_id: return live_alloc_count
 *                         (MR0) + live_bytes (MR1); refresh Prometheus ring
 *   0x83 OP_MP_RESET    — MR0=opcode, MR1=slot_id: clear slot stats on respawn
 *
 * Channels (local IDs):
 *   id=0 CH_IN:  incoming PPCs from controller / init_agent
 *   id=1 CH_OUT: notify watchdog on leak detection
 *   id=2 CH_WD:  PPC to watchdog for OP_WD_STATUS (label 0x53); shares CH_OUT
 *                until a dedicated watchdog PD is wired on channel 66
 *
 * Shared memory (256KB "mem_profiler_ring" at 0x80000 vaddr in PD):
 *   Prometheus-style text lines written at ring base on each OP_MP_SNAPSHOT;
 *   fields: slot_id, live_alloc_count, live_bytes, monotone_ticks.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"

/* ── Opcodes (MR0 field) ───────────────────────────────────────────────────── */
#define OP_MP_ALLOC     0x80u  /* MR1=slot_id, MR2=bytes */
#define OP_MP_FREE      0x81u  /* MR1=slot_id, MR2=bytes */
#define OP_MP_SNAPSHOT  0x82u  /* MR1=slot_id → MR0=live_alloc_count, MR1=live_bytes */
#define OP_MP_RESET     0x83u  /* MR1=slot_id: clear all slot stats */

/* Watchdog status opcode sent via CH_WD on leak (OP_WD_STATUS label 0x53) */
#define OP_WD_STATUS    0x53u

/* ── Channel local IDs ─────────────────────────────────────────────────────── */
#define CH_IN   0  /* PPCs from controller / init_agent */
#define CH_OUT  1  /* notify watchdog on leak detection */
#define CH_WD   1  /* PPC to watchdog (shares CH_OUT until dedicated watchdog PD) */

/* ── Leak detection ────────────────────────────────────────────────────────── */
#define LEAK_TICK_THRESHOLD  100u  /* consecutive alloc ticks with live_bytes growth */

/* ── Slot table ────────────────────────────────────────────────────────────── */
#define MAX_MP_SLOTS  16

typedef struct {
    uint32_t slot_id;
    uint32_t active;             /* 1 = registered */
    uint32_t live_alloc_count;   /* current live allocation count */
    uint32_t tick_monotonic;     /* alloc tick counter (incremented per OP_MP_ALLOC) */
    uint32_t monotone_ticks;     /* consecutive ticks where live_bytes > last_snapshot_bytes */
    uint32_t leak_alerted;       /* 1 after leak alert sent (reset on SNAPSHOT/RESET) */
    uint64_t live_bytes;         /* current live heap bytes */
    uint64_t last_snapshot_bytes;/* live_bytes at last OP_MP_SNAPSHOT (leak heuristic baseline) */
} mp_slot_t;

static mp_slot_t slots[MAX_MP_SLOTS];

/* ── Shared memory (256KB Prometheus ring) ─────────────────────────────────── */
uintptr_t mem_profiler_ring_vaddr;

#define MP_RING_BASE   ((volatile uint8_t *)mem_profiler_ring_vaddr)
#define MP_RING_SIZE   0x40000u  /* 256KB */

/* ── Helpers ───────────────────────────────────────────────────────────────── */
static void put_dec(uint32_t v) {
    if (v == 0) { microkit_dbg_puts("0"); return; }
    char buf[12]; int i = 11;
    buf[i] = '\0';
    while (v > 0 && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; }
    microkit_dbg_puts(&buf[i]);
}

/* Append ASCII decimal of a u32 into ring[pos..], return new pos */
static uint32_t ring_put_u32(volatile uint8_t *ring, uint32_t pos,
                              uint32_t lim, uint32_t v)
{
    char tmp[12]; int i = 11;
    tmp[i] = '\0';
    if (v == 0) { tmp[--i] = '0'; }
    else { while (v > 0 && i > 0) { tmp[--i] = '0' + (v % 10); v /= 10; } }
    for (; tmp[i] && pos < lim - 1; i++) ring[pos++] = (uint8_t)tmp[i];
    return pos;
}

/* Append ASCII decimal of a u64 into ring[pos..], return new pos */
static uint32_t ring_put_u64(volatile uint8_t *ring, uint32_t pos,
                              uint32_t lim, uint64_t v)
{
    char tmp[22]; int i = 21;
    tmp[i] = '\0';
    if (v == 0) { tmp[--i] = '0'; }
    else { while (v > 0 && i > 0) { tmp[--i] = '0' + (uint8_t)(v % 10); v /= 10; } }
    for (; tmp[i] && pos < lim - 1; i++) ring[pos++] = (uint8_t)tmp[i];
    return pos;
}

/* Append a literal C string into ring[pos..], return new pos */
static uint32_t ring_put_str(volatile uint8_t *ring, uint32_t pos,
                              uint32_t lim, const char *s)
{
    while (*s && pos < lim - 1) ring[pos++] = (uint8_t)*s++;
    return pos;
}

/* ── Slot lookup ───────────────────────────────────────────────────────────── */
static int find_slot(uint32_t slot_id) {
    for (int i = 0; i < MAX_MP_SLOTS; i++)
        if (slots[i].active && slots[i].slot_id == slot_id) return i;
    return -1;
}

static int find_free_slot(void) {
    for (int i = 0; i < MAX_MP_SLOTS; i++)
        if (!slots[i].active) return i;
    return -1;
}

/* ── Prometheus text snapshot → ring ──────────────────────────────────────── */
static void write_prometheus_ring(void) {
    volatile uint8_t *ring = MP_RING_BASE;
    uint32_t p = 0;
    uint32_t lim = MP_RING_SIZE;

    p = ring_put_str(ring, p, lim, "# agentOS mem_profiler snapshot\n");

    p = ring_put_str(ring, p, lim,
        "# HELP wasm_live_alloc_count Live allocation count per WASM slot\n"
        "# TYPE wasm_live_alloc_count gauge\n");
    for (int i = 0; i < MAX_MP_SLOTS; i++) {
        if (!slots[i].active) continue;
        p = ring_put_str(ring, p, lim, "wasm_live_alloc_count{slot=\"");
        p = ring_put_u32(ring, p, lim, slots[i].slot_id);
        p = ring_put_str(ring, p, lim, "\"} ");
        p = ring_put_u32(ring, p, lim, slots[i].live_alloc_count);
        p = ring_put_str(ring, p, lim, "\n");
    }

    p = ring_put_str(ring, p, lim,
        "# HELP wasm_live_bytes Live heap bytes per WASM slot\n"
        "# TYPE wasm_live_bytes gauge\n");
    for (int i = 0; i < MAX_MP_SLOTS; i++) {
        if (!slots[i].active) continue;
        p = ring_put_str(ring, p, lim, "wasm_live_bytes{slot=\"");
        p = ring_put_u32(ring, p, lim, slots[i].slot_id);
        p = ring_put_str(ring, p, lim, "\"} ");
        p = ring_put_u64(ring, p, lim, slots[i].live_bytes);
        p = ring_put_str(ring, p, lim, "\n");
    }

    p = ring_put_str(ring, p, lim,
        "# HELP wasm_monotone_ticks Consecutive leak ticks per WASM slot\n"
        "# TYPE wasm_monotone_ticks gauge\n");
    for (int i = 0; i < MAX_MP_SLOTS; i++) {
        if (!slots[i].active) continue;
        p = ring_put_str(ring, p, lim, "wasm_monotone_ticks{slot=\"");
        p = ring_put_u32(ring, p, lim, slots[i].slot_id);
        p = ring_put_str(ring, p, lim, "\"} ");
        p = ring_put_u32(ring, p, lim, slots[i].monotone_ticks);
        p = ring_put_str(ring, p, lim, "\n");
    }

    if (p < lim) ring[p] = 0;  /* null-terminate */
}

/* ── Notify watchdog of a leak ─────────────────────────────────────────────── */
static void alert_watchdog(uint32_t slot_id) {
    microkit_mr_set(0, MSG_MEM_LEAK_ALERT);
    microkit_mr_set(1, slot_id);
    microkit_mr_set(2, LEAK_TICK_THRESHOLD);
    microkit_notify(CH_OUT);

    microkit_dbg_puts("[mem_profiler] LEAK slot=");
    put_dec(slot_id);
    microkit_dbg_puts(" monotone_ticks=");
    put_dec(LEAK_TICK_THRESHOLD);
    microkit_dbg_puts("\n");
}

/* ── Handle PPC request ────────────────────────────────────────────────────── */
static microkit_msginfo handle_request(microkit_msginfo msg) {
    uint32_t op      = (uint32_t)microkit_mr_get(0);
    uint32_t slot_id = (uint32_t)microkit_mr_get(1);

    switch (op) {

    case OP_MP_ALLOC: {
        uint32_t bytes = (uint32_t)microkit_mr_get(2);

        int idx = find_slot(slot_id);
        if (idx < 0) {
            /* Auto-register on first call (bytes=0 is the registration idiom) */
            idx = find_free_slot();
            if (idx < 0) {
                microkit_dbg_puts("[mem_profiler] ERR: slot table full\n");
                microkit_mr_set(0, 0xE1u);
                return microkit_msginfo_new(0, 1);
            }
            slots[idx].slot_id             = slot_id;
            slots[idx].active              = 1;
            slots[idx].live_alloc_count    = 0;
            slots[idx].tick_monotonic      = 0;
            slots[idx].monotone_ticks      = 0;
            slots[idx].leak_alerted        = 0;
            slots[idx].live_bytes          = 0;
            slots[idx].last_snapshot_bytes = 0;
            microkit_dbg_puts("[mem_profiler] register slot=");
            put_dec(slot_id);
            microkit_dbg_puts("\n");
        }

        slots[idx].live_alloc_count++;
        slots[idx].live_bytes     += bytes;
        slots[idx].tick_monotonic++;

        /* Leak heuristic: count consecutive ticks where heap grows above baseline */
        if (slots[idx].live_bytes > slots[idx].last_snapshot_bytes) {
            slots[idx].monotone_ticks++;
            if (slots[idx].monotone_ticks >= LEAK_TICK_THRESHOLD &&
                !slots[idx].leak_alerted) {
                slots[idx].leak_alerted = 1;
                alert_watchdog(slot_id);
            }
        } else {
            slots[idx].monotone_ticks = 0;
        }

        microkit_mr_set(0, 0u);
        return microkit_msginfo_new(0, 1);
    }

    case OP_MP_FREE: {
        uint32_t bytes = (uint32_t)microkit_mr_get(2);

        int idx = find_slot(slot_id);
        if (idx < 0) {
            microkit_mr_set(0, 0xE2u);
            return microkit_msginfo_new(0, 1);
        }

        if (slots[idx].live_alloc_count > 0) slots[idx].live_alloc_count--;
        if (slots[idx].live_bytes >= bytes) slots[idx].live_bytes -= bytes;
        else                                slots[idx].live_bytes  = 0;

        /* Healthy free activity: if heap drops to/below baseline, reset counter */
        if (slots[idx].live_bytes <= slots[idx].last_snapshot_bytes) {
            slots[idx].monotone_ticks = 0;
            slots[idx].leak_alerted   = 0;
        }

        microkit_mr_set(0, 0u);
        return microkit_msginfo_new(0, 1);
    }

    case OP_MP_SNAPSHOT: {
        int idx = find_slot(slot_id);
        if (idx < 0) {
            microkit_mr_set(0, 0xE2u);
            return microkit_msginfo_new(0, 1);
        }

        /* Update baseline; reset leak counter so next window starts fresh */
        slots[idx].last_snapshot_bytes = slots[idx].live_bytes;
        slots[idx].monotone_ticks      = 0;
        slots[idx].leak_alerted        = 0;

        write_prometheus_ring();

        microkit_mr_set(0, slots[idx].live_alloc_count);
        microkit_mr_set(1, (uint32_t)slots[idx].live_bytes);
        return microkit_msginfo_new(0, 2);
    }

    case OP_MP_RESET: {
        int idx = find_slot(slot_id);
        if (idx < 0) {
            microkit_mr_set(0, 0xE2u);
            return microkit_msginfo_new(0, 1);
        }

        slots[idx].live_alloc_count    = 0;
        slots[idx].tick_monotonic      = 0;
        slots[idx].monotone_ticks      = 0;
        slots[idx].leak_alerted        = 0;
        slots[idx].live_bytes          = 0;
        slots[idx].last_snapshot_bytes = 0;

        microkit_dbg_puts("[mem_profiler] reset slot=");
        put_dec(slot_id);
        microkit_dbg_puts("\n");

        microkit_mr_set(0, 0u);
        return microkit_msginfo_new(0, 1);
    }

    default:
        microkit_dbg_puts("[mem_profiler] WARN: unknown op\n");
        microkit_mr_set(0, 0xFFu);
        return microkit_msginfo_new(0, 1);
    }

    (void)msg;
}

/* ── Microkit entry points ─────────────────────────────────────────────────── */
void init(void) {
    agentos_log_boot("mem_profiler");

    for (int i = 0; i < MAX_MP_SLOTS; i++)
        slots[i].active = 0;

    /* Seed ring with placeholder until first snapshot */
    const char *hdr = "# agentOS mem_profiler: no snapshots yet\n";
    volatile uint8_t *ring = MP_RING_BASE;
    for (int i = 0; hdr[i]; i++) ring[i] = (uint8_t)hdr[i];
    ring[41] = 0;

    microkit_dbg_puts("[mem_profiler] Ready — priority 108, passive, ");
    put_dec(MAX_MP_SLOTS);
    microkit_dbg_puts(" WASM slot tracking slots\n");
}

microkit_msginfo protected(microkit_channel channel, microkit_msginfo msg) {
    (void)channel;
    return handle_request(msg);
}

void notified(microkit_channel ch) {
    /* No periodic tick needed; leak detection is driven by OP_MP_ALLOC calls */
    (void)ch;
}
