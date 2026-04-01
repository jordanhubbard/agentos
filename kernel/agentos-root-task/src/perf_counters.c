/*
 * agentOS PerfCounters Protection Domain
 *
 * Passive PD (priority 95).  Maintains continuous, low-overhead latency
 * histograms for every inter-PD PPC channel pair in the system.
 *
 * Architecture
 * ────────────
 * PDs call OP_PERF_BEGIN before a PPC and OP_PERF_END after.  Both calls
 * are PPCs into this PD (pp=true on the caller's channel end).
 *
 * OP_PERF_BEGIN records a timestamp (ARM64 cntvct_el0 or RISC-V rdcycle).
 * OP_PERF_END computes the delta and inserts it into the histogram for
 * (caller_pd, callee_pd) bucket.
 *
 * Histograms
 * ──────────
 * Each PD-pair has 16 log-scale buckets covering 100 ns – 1 ms:
 *   bucket 0:  < 100 ns
 *   bucket 1:  100 ns – 200 ns
 *   bucket 2:  200 ns – 500 ns
 *   bucket 3:  500 ns – 1 µs
 *   bucket 4:    1 µs –   2 µs
 *   bucket 5:    2 µs –   5 µs
 *   bucket 6:    5 µs –  10 µs
 *   bucket 7:   10 µs –  20 µs
 *   bucket 8:   20 µs –  50 µs
 *   bucket 9:   50 µs – 100 µs
 *   bucket 10: 100 µs – 200 µs
 *   bucket 11: 200 µs – 500 µs
 *   bucket 12: 500 µs –   1 ms
 *   bucket 13:   1 ms –   5 ms
 *   bucket 14:   5 ms –  50 ms
 *   bucket 15: ≥ 50 ms  (overflow)
 *
 * Exposed via shared ring buffer (perf_ring MR) as packed 64-byte records.
 * The metrics_exporter PD reads these and forwards to Prometheus/Grafana.
 *
 * Messages (MR0 opcode)
 * ─────────────────────
 *   OP_PERF_BEGIN  (0xC0): MR1=caller_pd_id, MR2=callee_pd_id → token in MR0
 *   OP_PERF_END    (0xC1): MR1=token (from BEGIN), MR2=callee_pd_id → latency ns in MR0
 *   OP_PERF_QUERY  (0xC2): MR1=caller_pd, MR2=callee_pd → fill histogram in reply MRs
 *   OP_PERF_RESET  (0xC3): clear all histograms (for test harnesses)
 *   OP_PERF_EXPORT (0xC4): write all histograms to perf_ring MR for bulk export
 *
 * Channel assignments (see agentos.system):
 *   id=0: controller → perf_counters  (pp=true on controller end)
 *   id=1..8: worker_0..7 → perf_counters (pp=true on worker end)
 *
 * Timer access
 * ────────────
 * RISC-V: rdcycle (CSR 0xC00) via __asm__ volatile
 * AArch64: cntvct_el0 system register
 * Assumes ~1 GHz CPU for ns conversion (adjust via PERF_CYCLES_PER_NS).
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "prio_inherit.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ── Configuration ─────────────────────────────────────────────────────── */

#define MAX_PD_PAIRS      64    /* max unique (caller, callee) pairs tracked */
#define HIST_BUCKETS      16    /* latency histogram buckets */
#define MAX_PENDING       16    /* max in-flight BEGIN tokens per PD */

/* CPU frequency assumption for ns conversion (1 GHz default, 250 MHz for QEMU) */
#ifndef PERF_CYCLES_PER_NS
#  ifdef QEMU_VIRT
#    define PERF_CYCLES_PER_NS  1ULL   /* QEMU rdcycle ≈ ns */
#  else
#    define PERF_CYCLES_PER_NS  1ULL   /* 1 GHz: 1 cycle = 1 ns */
#  endif
#endif

/* Opcode constants */
#define OP_PERF_BEGIN    0xC0
#define OP_PERF_END      0xC1
#define OP_PERF_QUERY    0xC2
#define OP_PERF_RESET    0xC3
#define OP_PERF_EXPORT   0xC4

/* Channel IDs for this PD (perf_counters sees these from its perspective) */
#define CH_CONTROLLER_PC  0   /* controller ↔ perf_counters */
/* worker channels: 1..8 match worker_N id=10 end toward perf_counters */

/* ── Histogram bucket thresholds (in nanoseconds) ─────────────────────── */

static const uint64_t bucket_ns[HIST_BUCKETS] = {
    100, 200, 500, 1000, 2000, 5000, 10000, 20000,
    50000, 100000, 200000, 500000, 1000000, 5000000, 50000000,
    UINT64_MAX  /* overflow bucket */
};

/* ── Data structures ───────────────────────────────────────────────────── */

typedef struct {
    uint32_t caller_pd;
    uint32_t callee_pd;
    uint64_t call_count;
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t buckets[HIST_BUCKETS];
} PerfEntry;

/* Pending BEGIN tokens (one per in-flight call) */
typedef struct {
    uint32_t token;         /* unique token returned to caller */
    uint32_t caller_pd;
    uint32_t callee_pd;
    uint64_t start_cycles;
    bool     active;
} PendingEntry;

/* Global state */
static PerfEntry    perf_table[MAX_PD_PAIRS];
static uint32_t     perf_count  = 0;
static PendingEntry pending[MAX_PENDING];
static uint32_t     next_token  = 1;

/* Shared ring buffer for bulk export (mapped via perf_ring MR) */
uintptr_t perf_ring_vaddr;

/* ── Timer ─────────────────────────────────────────────────────────────── */

static inline uint64_t read_cycles(void) {
#if defined(__riscv)
    uint64_t cycles;
    __asm__ volatile ("rdcycle %0" : "=r"(cycles));
    return cycles;
#elif defined(__aarch64__)
    uint64_t val;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    return 0;  /* unsupported arch: latency = 0 */
#endif
}

static inline uint64_t cycles_to_ns(uint64_t cycles) {
    return cycles / PERF_CYCLES_PER_NS;
}

/* ── Histogram helpers ─────────────────────────────────────────────────── */

static PerfEntry *find_or_create_entry(uint32_t caller_pd, uint32_t callee_pd) {
    for (uint32_t i = 0; i < perf_count; i++) {
        if (perf_table[i].caller_pd == caller_pd &&
            perf_table[i].callee_pd == callee_pd)
            return &perf_table[i];
    }
    if (perf_count >= MAX_PD_PAIRS) return NULL;
    PerfEntry *e = &perf_table[perf_count++];
    memset(e, 0, sizeof(*e));
    e->caller_pd = caller_pd;
    e->callee_pd = callee_pd;
    e->min_ns    = UINT64_MAX;
    return e;
}

static void record_latency(PerfEntry *e, uint64_t ns) {
    e->call_count++;
    e->total_ns += ns;
    if (ns < e->min_ns) e->min_ns = ns;
    if (ns > e->max_ns) e->max_ns = ns;
    for (int b = 0; b < HIST_BUCKETS; b++) {
        if (ns < bucket_ns[b]) { e->buckets[b]++; return; }
    }
    e->buckets[HIST_BUCKETS - 1]++;
}

static uint64_t percentile(const PerfEntry *e, uint32_t pct) {
    if (e->call_count == 0) return 0;
    uint64_t target = (e->call_count * pct) / 100;
    uint64_t cum = 0;
    for (int b = 0; b < HIST_BUCKETS - 1; b++) {
        cum += e->buckets[b];
        if (cum >= target) return bucket_ns[b];
    }
    return e->max_ns;
}

/* ── Ring buffer export ─────────────────────────────────────────────────── */

/*
 * Export record layout (64 bytes, packed):
 *   [0..3]   caller_pd   (u32)
 *   [4..7]   callee_pd   (u32)
 *   [8..15]  call_count  (u64)
 *   [16..23] total_ns    (u64)
 *   [24..31] min_ns      (u64)
 *   [32..39] max_ns      (u64)
 *   [40..47] p50_ns      (u64)
 *   [48..55] p95_ns      (u64)
 *   [56..63] p99_ns      (u64)
 */
#define PERF_EXPORT_RECORD_SIZE 64

static void export_to_ring(void) {
    if (!perf_ring_vaddr) return;
    uint8_t *ring = (uint8_t *)perf_ring_vaddr;
    for (uint32_t i = 0; i < perf_count; i++) {
        uint8_t *rec = ring + i * PERF_EXPORT_RECORD_SIZE;
        const PerfEntry *e = &perf_table[i];
        uint64_t p50 = percentile(e, 50);
        uint64_t p95 = percentile(e, 95);
        uint64_t p99 = percentile(e, 99);
        /* Write fields (no memcpy to avoid alignment issues) */
        uint32_t *u32p = (uint32_t *)rec;
        uint64_t *u64p = (uint64_t *)(rec + 8);
        u32p[0] = e->caller_pd;
        u32p[1] = e->callee_pd;
        u64p[0] = e->call_count;
        u64p[1] = e->total_ns;
        u64p[2] = e->min_ns;
        u64p[3] = e->max_ns;
        u64p[4] = p50;
        u64p[5] = p95;
        u64p[6] = p99;
    }
    /* Write a sentinel record to mark end */
    if (perf_count < MAX_PD_PAIRS) {
        memset(ring + perf_count * PERF_EXPORT_RECORD_SIZE, 0,
               PERF_EXPORT_RECORD_SIZE);
    }
}

/* ── OP_PERF_BEGIN ─────────────────────────────────────────────────────── */

static uint32_t handle_begin(uint32_t caller_pd, uint32_t callee_pd) {
    /* Find a free pending slot */
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!pending[i].active) {
            pending[i].active       = true;
            pending[i].caller_pd    = caller_pd;
            pending[i].callee_pd    = callee_pd;
            pending[i].start_cycles = read_cycles();
            pending[i].token        = next_token++;
            if (next_token == 0) next_token = 1;  /* wrap, skip 0 */
            return pending[i].token;
        }
    }
    return 0;  /* pending full — caller gets token=0 (no END needed) */
}

/* ── OP_PERF_END ───────────────────────────────────────────────────────── */

static uint64_t handle_end(uint32_t token, uint32_t callee_pd) {
    if (token == 0) return 0;
    uint64_t now = read_cycles();
    for (int i = 0; i < MAX_PENDING; i++) {
        if (pending[i].active && pending[i].token == token) {
            uint64_t elapsed_cycles = now - pending[i].start_cycles;
            uint64_t ns = cycles_to_ns(elapsed_cycles);
            PerfEntry *e = find_or_create_entry(pending[i].caller_pd,
                                                callee_pd);
            if (e) record_latency(e, ns);
            pending[i].active = false;
            return ns;
        }
    }
    return 0;
}

/* ── OP_PERF_QUERY ─────────────────────────────────────────────────────── */

static void handle_query(uint32_t caller_pd, uint32_t callee_pd) {
    PerfEntry *e = find_or_create_entry(caller_pd, callee_pd);
    if (!e || e->call_count == 0) {
        microkit_mr_set(0, 0);  /* call_count = 0 */
        return;
    }
    /* Return: MR0=call_count (lo), MR1=call_count (hi),
     *         MR2=p50_ns (lo), MR3=p95_ns (lo), MR4=p99_ns (lo) */
    microkit_mr_set(0, (uint32_t)(e->call_count & 0xFFFFFFFF));
    microkit_mr_set(1, (uint32_t)(e->call_count >> 32));
    microkit_mr_set(2, (uint32_t)(percentile(e, 50) & 0xFFFFFFFF));
    microkit_mr_set(3, (uint32_t)(percentile(e, 95) & 0xFFFFFFFF));
    microkit_mr_set(4, (uint32_t)(percentile(e, 99) & 0xFFFFFFFF));
}

/* ── Microkit entry points ─────────────────────────────────────────────── */

void init(void) {
    agentos_log_boot("perf_counters");
    memset(perf_table, 0, sizeof(perf_table));
    memset(pending,    0, sizeof(pending));
    microkit_dbg_puts("[perf_counters] IPC latency histograms ready\n");
    microkit_dbg_puts("[perf_counters] Buckets: 100ns..1ms (16 log-scale)\n");
}

void notified(microkit_channel ch) {
    /* perf_counters is passive — notifications are unexpected, ignore */
    (void)ch;
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)ch;
    uint32_t op         = (uint32_t)microkit_msginfo_get_label(msginfo);
    uint32_t caller_pd  = (uint32_t)microkit_mr_get(1);
    uint32_t callee_pd  = (uint32_t)microkit_mr_get(2);

    switch (op) {
        case OP_PERF_BEGIN: {
            uint32_t token = handle_begin(caller_pd, callee_pd);
            microkit_mr_set(0, token);
            return microkit_msginfo_new(0, 1);
        }
        case OP_PERF_END: {
            uint32_t token = (uint32_t)microkit_mr_get(1);
            uint64_t ns    = handle_end(token, callee_pd);
            microkit_mr_set(0, (uint32_t)(ns & 0xFFFFFFFF));
            microkit_mr_set(1, (uint32_t)(ns >> 32));
            return microkit_msginfo_new(0, 2);
        }
        case OP_PERF_QUERY: {
            handle_query(caller_pd, callee_pd);
            return microkit_msginfo_new(0, 5);
        }
        case OP_PERF_RESET: {
            memset(perf_table, 0, sizeof(perf_table));
            memset(pending,    0, sizeof(pending));
            perf_count  = 0;
            next_token  = 1;
            microkit_mr_set(0, 1);
            microkit_dbg_puts("[perf_counters] All histograms reset\n");
            return microkit_msginfo_new(0, 1);
        }
        case OP_PERF_EXPORT: {
            export_to_ring();
            microkit_mr_set(0, perf_count);
            return microkit_msginfo_new(0, 1);
        }
        default:
            microkit_mr_set(0, 0xDEAD);
            return microkit_msginfo_new(1, 1);
    }
}
