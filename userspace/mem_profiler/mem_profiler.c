/*
 * agentOS mem_profiler — per-slot WASM heap allocation tracking + leak detection
 *
 * Passive PD, priority 108.
 *
 * Protocol:
 *   OP_MEM_ALLOC  (0x60) — MR1=slot_id, MR2=size, MR3=ptr_hint: record allocation
 *   OP_MEM_FREE   (0x61) — MR1=slot_id, MR2=ptr_hint, MR3=size: record free
 *   OP_MEM_QUERY  (0x62) — MR1=slot_id → MR0=bytes_allocated, MR1=alloc_count,
 *                          MR2=peak_bytes, MR3=last_op_tick
 *   OP_MEM_ALERT  (0x63) — outbound notify (CH_ALERT): sent to controller when
 *                          slot exceeds MEM_QUOTA_BYTES (2MB) or leak detected
 *   OP_MEM_RESET  (0x64) — MR1=slot_id: clear all tracking for slot (slot reclaim)
 *
 * State: table[16] of {slot_id, bytes_allocated, alloc_count, peak_bytes, last_op_tick}
 *
 * Quota: MEM_QUOTA_BYTES = 2MB per slot.  Alert sent on first OP_MEM_ALLOC that
 *        pushes bytes_allocated over the limit; re-armed on OP_MEM_RESET.
 *
 * Leak detection: if LEAK_TICK_THRESHOLD consecutive OP_MEM_ALLOC ticks occur
 *   without any intervening OP_MEM_FREE for that slot (i.e. alloc_count keeps
 *   growing with no frees), the slot is flagged leaked and OP_MEM_ALERT is sent.
 *
 * Channels (local IDs from mem_profiler's perspective):
 *   id=0 CH_IN:    incoming PPCs from controller / workers
 *   id=1 CH_ALERT: notify controller on quota exceeded or leak detected
 *
 * Shared memory (256KB mem_profiler_ring):
 *   Simple header + packed slot table, readable by controller via shared MR.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"

/* ── Opcodes (MR0 field) ───────────────────────────────────────────────────── */
#define OP_MEM_ALLOC  0x60u  /* MR1=slot_id, MR2=size, MR3=ptr_hint */
#define OP_MEM_FREE   0x61u  /* MR1=slot_id, MR2=ptr_hint, MR3=size */
#define OP_MEM_QUERY  0x62u  /* MR1=slot_id → MR0=bytes_alloc, MR1=alloc_cnt, MR2=peak, MR3=tick */
#define OP_MEM_ALERT  0x63u  /* outbound only — not handled as inbound op */
#define OP_MEM_RESET  0x64u  /* MR1=slot_id */

/* Alert sub-types sent in MR0 on CH_ALERT */
#define ALERT_QUOTA_EXCEEDED  0x6301u  /* MR1=slot_id, MR2=bytes_allocated (low 32) */
#define ALERT_LEAK_DETECTED   0x6302u  /* MR1=slot_id, MR2=alloc_count */

/* ── Channel local IDs ─────────────────────────────────────────────────────── */
#define CH_IN     0  /* PPCs from controller / workers */
#define CH_ALERT  1  /* notify controller: quota or leak */

/* ── Quota / leak thresholds ───────────────────────────────────────────────── */
#define MEM_QUOTA_BYTES       (2u * 1024u * 1024u)  /* 2MB per slot */
#define LEAK_TICK_THRESHOLD   200u                  /* alloc-only ticks before leak flag */

/* ── Slot table ────────────────────────────────────────────────────────────── */
#define MAX_MEM_SLOTS  16

typedef struct {
    uint32_t slot_id;
    uint32_t active;           /* 1 = registered */
    uint32_t alloc_count;      /* current live allocation count */
    uint32_t quota_alerted;    /* 1 after quota alert sent; cleared on RESET */
    uint32_t leak_alerted;     /* 1 after leak alert sent; cleared on RESET */
    uint32_t dry_alloc_ticks;  /* consecutive OP_MEM_ALLOC ticks with no FREE */
    uint32_t last_op_tick;     /* global tick of last alloc/free on this slot */
    uint32_t _pad;
    uint64_t bytes_allocated;  /* current live heap bytes */
    uint64_t peak_bytes;       /* high-water mark of bytes_allocated */
} mem_slot_t;

static mem_slot_t table[MAX_MEM_SLOTS];

/* Global monotonic tick — incremented on every OP_MEM_ALLOC or OP_MEM_FREE */
static uint32_t global_tick = 0;

/* ── Shared memory (256KB Prometheus / status ring) ─────────────────────────── */
uintptr_t mem_profiler_ring_vaddr;

#define MP_RING_BASE  ((volatile uint8_t *)mem_profiler_ring_vaddr)
#define MP_RING_SIZE  0x40000u  /* 256KB */

/* ── Debug helpers ─────────────────────────────────────────────────────────── */
static void put_dec(uint32_t v) {
    if (v == 0) { microkit_dbg_puts("0"); return; }
    char buf[12]; int i = 11;
    buf[i] = '\0';
    while (v > 0 && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; }
    microkit_dbg_puts(&buf[i]);
}

/* ── Ring helpers ──────────────────────────────────────────────────────────── */
static uint32_t ring_put_str(volatile uint8_t *r, uint32_t p, uint32_t lim,
                              const char *s) {
    while (*s && p < lim - 1) r[p++] = (uint8_t)*s++;
    return p;
}
static uint32_t ring_put_u32(volatile uint8_t *r, uint32_t p, uint32_t lim,
                              uint32_t v) {
    char tmp[12]; int i = 11;
    tmp[i] = '\0';
    if (v == 0) { tmp[--i] = '0'; }
    else { while (v > 0 && i > 0) { tmp[--i] = '0' + (v % 10); v /= 10; } }
    for (; tmp[i] && p < lim - 1; i++) r[p++] = (uint8_t)tmp[i];
    return p;
}
static uint32_t ring_put_u64(volatile uint8_t *r, uint32_t p, uint32_t lim,
                              uint64_t v) {
    char tmp[22]; int i = 21;
    tmp[i] = '\0';
    if (v == 0) { tmp[--i] = '0'; }
    else { while (v > 0 && i > 0) { tmp[--i] = '0' + (uint8_t)(v % 10); v /= 10; } }
    for (; tmp[i] && p < lim - 1; i++) r[p++] = (uint8_t)tmp[i];
    return p;
}

/* ── Write Prometheus-style snapshot to ring ────────────────────────────────── */
static void write_ring_snapshot(void) {
    volatile uint8_t *ring = MP_RING_BASE;
    uint32_t p = 0, lim = MP_RING_SIZE;

    p = ring_put_str(ring, p, lim, "# agentOS mem_profiler snapshot\n");
    p = ring_put_str(ring, p, lim,
        "# HELP wasm_bytes_allocated Live heap bytes per WASM slot\n"
        "# TYPE wasm_bytes_allocated gauge\n");
    for (int i = 0; i < MAX_MEM_SLOTS; i++) {
        if (!table[i].active) continue;
        p = ring_put_str(ring, p, lim, "wasm_bytes_allocated{slot=\"");
        p = ring_put_u32(ring, p, lim, table[i].slot_id);
        p = ring_put_str(ring, p, lim, "\"} ");
        p = ring_put_u64(ring, p, lim, table[i].bytes_allocated);
        p = ring_put_str(ring, p, lim, "\n");
    }
    p = ring_put_str(ring, p, lim,
        "# HELP wasm_alloc_count Live allocation count per WASM slot\n"
        "# TYPE wasm_alloc_count gauge\n");
    for (int i = 0; i < MAX_MEM_SLOTS; i++) {
        if (!table[i].active) continue;
        p = ring_put_str(ring, p, lim, "wasm_alloc_count{slot=\"");
        p = ring_put_u32(ring, p, lim, table[i].slot_id);
        p = ring_put_str(ring, p, lim, "\"} ");
        p = ring_put_u32(ring, p, lim, table[i].alloc_count);
        p = ring_put_str(ring, p, lim, "\n");
    }
    p = ring_put_str(ring, p, lim,
        "# HELP wasm_peak_bytes Peak heap bytes per WASM slot\n"
        "# TYPE wasm_peak_bytes gauge\n");
    for (int i = 0; i < MAX_MEM_SLOTS; i++) {
        if (!table[i].active) continue;
        p = ring_put_str(ring, p, lim, "wasm_peak_bytes{slot=\"");
        p = ring_put_u32(ring, p, lim, table[i].slot_id);
        p = ring_put_str(ring, p, lim, "\"} ");
        p = ring_put_u64(ring, p, lim, table[i].peak_bytes);
        p = ring_put_str(ring, p, lim, "\n");
    }
    if (p < lim) ring[p] = 0;
}

/* ── Slot lookup ───────────────────────────────────────────────────────────── */
static int find_slot(uint32_t slot_id) {
    for (int i = 0; i < MAX_MEM_SLOTS; i++)
        if (table[i].active && table[i].slot_id == slot_id) return i;
    return -1;
}
static int find_free_slot(void) {
    for (int i = 0; i < MAX_MEM_SLOTS; i++)
        if (!table[i].active) return i;
    return -1;
}

/* ── Alert helper ──────────────────────────────────────────────────────────── */
static void send_alert(uint32_t alert_type, uint32_t slot_id, uint32_t val) {
    microkit_mr_set(0, alert_type);
    microkit_mr_set(1, slot_id);
    microkit_mr_set(2, val);
    microkit_notify(CH_ALERT);
    microkit_dbg_puts("[mem_profiler] ALERT 0x");
    /* minimal hex print */
    uint32_t a = alert_type;
    char hx[9]; int hi = 8; hx[hi] = '\0';
    do { hx[--hi] = "0123456789ABCDEF"[a & 0xF]; a >>= 4; } while (a && hi > 0);
    microkit_dbg_puts(&hx[hi]);
    microkit_dbg_puts(" slot=");
    put_dec(slot_id);
    microkit_dbg_puts(" val=");
    put_dec(val);
    microkit_dbg_puts("\n");
}

/* ── Handle PPC request ────────────────────────────────────────────────────── */
static microkit_msginfo handle_request(microkit_msginfo msg) {
    uint32_t op      = (uint32_t)microkit_mr_get(0);
    uint32_t slot_id = (uint32_t)microkit_mr_get(1);

    switch (op) {

    /* ── OP_MEM_ALLOC ─────────────────────────────────────────────────────── */
    case OP_MEM_ALLOC: {
        uint32_t size = (uint32_t)microkit_mr_get(2);
        /* MR3=ptr_hint: recorded in log but not needed for byte tracking */

        int idx = find_slot(slot_id);
        if (idx < 0) {
            /* Auto-register on first alloc for an unknown slot */
            idx = find_free_slot();
            if (idx < 0) {
                microkit_dbg_puts("[mem_profiler] ERR: slot table full\n");
                microkit_mr_set(0, 0xE1u);
                return microkit_msginfo_new(0, 1);
            }
            table[idx].slot_id         = slot_id;
            table[idx].active          = 1;
            table[idx].alloc_count     = 0;
            table[idx].quota_alerted   = 0;
            table[idx].leak_alerted    = 0;
            table[idx].dry_alloc_ticks = 0;
            table[idx].last_op_tick    = global_tick;
            table[idx].bytes_allocated = 0;
            table[idx].peak_bytes      = 0;
            microkit_dbg_puts("[mem_profiler] auto-register slot=");
            put_dec(slot_id);
            microkit_dbg_puts("\n");
        }

        table[idx].alloc_count++;
        table[idx].bytes_allocated += size;
        if (table[idx].bytes_allocated > table[idx].peak_bytes)
            table[idx].peak_bytes = table[idx].bytes_allocated;
        table[idx].dry_alloc_ticks++;
        table[idx].last_op_tick = global_tick++;

        /* Quota check: alert controller on first crossing */
        if (table[idx].bytes_allocated > MEM_QUOTA_BYTES &&
            !table[idx].quota_alerted) {
            table[idx].quota_alerted = 1;
            send_alert(ALERT_QUOTA_EXCEEDED, slot_id,
                       (uint32_t)table[idx].bytes_allocated);
        }

        /* Leak check: consecutive allocs with no free */
        if (table[idx].dry_alloc_ticks >= LEAK_TICK_THRESHOLD &&
            !table[idx].leak_alerted) {
            table[idx].leak_alerted = 1;
            send_alert(ALERT_LEAK_DETECTED, slot_id, table[idx].alloc_count);
        }

        microkit_mr_set(0, 0u);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_MEM_FREE ──────────────────────────────────────────────────────── */
    case OP_MEM_FREE: {
        /* MR2=ptr_hint (informational), MR3=size (optional, 0 = unknown) */
        uint32_t size = (uint32_t)microkit_mr_get(3);

        int idx = find_slot(slot_id);
        if (idx < 0) {
            microkit_mr_set(0, 0xE2u);
            return microkit_msginfo_new(0, 1);
        }

        if (table[idx].alloc_count > 0) table[idx].alloc_count--;
        if (size > 0) {
            if (table[idx].bytes_allocated >= size)
                table[idx].bytes_allocated -= size;
            else
                table[idx].bytes_allocated = 0;
        }

        /* Reset leak window: a free shows the slot is not stuck */
        table[idx].dry_alloc_ticks = 0;
        table[idx].leak_alerted    = 0;
        table[idx].last_op_tick    = global_tick++;

        /* Re-arm quota alert if bytes dropped back below threshold */
        if (table[idx].bytes_allocated <= MEM_QUOTA_BYTES)
            table[idx].quota_alerted = 0;

        microkit_mr_set(0, 0u);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_MEM_QUERY ─────────────────────────────────────────────────────── */
    case OP_MEM_QUERY: {
        int idx = find_slot(slot_id);
        if (idx < 0) {
            microkit_mr_set(0, 0xE2u);
            return microkit_msginfo_new(0, 1);
        }

        /* Refresh ring on every query */
        write_ring_snapshot();

        microkit_mr_set(0, (uint32_t)table[idx].bytes_allocated);
        microkit_mr_set(1, table[idx].alloc_count);
        microkit_mr_set(2, (uint32_t)table[idx].peak_bytes);
        microkit_mr_set(3, table[idx].last_op_tick);
        return microkit_msginfo_new(0, 4);
    }

    /* ── OP_MEM_RESET ─────────────────────────────────────────────────────── */
    case OP_MEM_RESET: {
        int idx = find_slot(slot_id);
        if (idx < 0) {
            microkit_mr_set(0, 0xE2u);
            return microkit_msginfo_new(0, 1);
        }

        table[idx].alloc_count     = 0;
        table[idx].quota_alerted   = 0;
        table[idx].leak_alerted    = 0;
        table[idx].dry_alloc_ticks = 0;
        table[idx].last_op_tick    = global_tick;
        table[idx].bytes_allocated = 0;
        table[idx].peak_bytes      = 0;

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

    for (int i = 0; i < MAX_MEM_SLOTS; i++) {
        table[i].active = 0;
    }

    /* Seed ring with placeholder */
    const char *hdr = "# agentOS mem_profiler: no data yet\n";
    volatile uint8_t *ring = MP_RING_BASE;
    for (int i = 0; hdr[i]; i++) ring[i] = (uint8_t)hdr[i];
    ring[36] = 0;

    microkit_dbg_puts("[mem_profiler] Ready — priority 108, passive, 16 slots, "
                      "2MB quota, leak@200 ticks\n");
}

microkit_msginfo protected(microkit_channel channel, microkit_msginfo msg) {
    (void)channel;
    return handle_request(msg);
}

void notified(microkit_channel ch) {
    (void)ch;
}
