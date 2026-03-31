/*
 * agentOS Bench Agent — IPC batch throughput test
 *
 * Compares single-publish vs batch-publish to the EventBus PD.
 * Runs 100 events through each path and reports cycle counts.
 *
 * Protection domain layout (must match agentos.system):
 *   CH_BENCH_EVENTBUS   — this PD's PPC channel to event_bus
 *
 * The bench agent shares the eventbus_ring memory region (read/write)
 * so it can populate the batch staging area directly before calling
 * MSG_EVENTBUS_PUBLISH_BATCH.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"

/* Channel ID to the EventBus from this PD (must match .system file) */
#define CH_BENCH_EVENTBUS  3

/* Number of events in each benchmark run */
#define BENCH_MSG_COUNT    100

/* Shared ring memory — set by Microkit via setvar_vaddr */
uintptr_t eventbus_ring_vaddr;

/* ── Cycle counter helpers ─────────────────────────────────────────────── */

static inline uint64_t bench_cycles(void) {
#if defined(__aarch64__)
    uint64_t t;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
    return t;
#elif defined(__riscv)
    uint64_t t;
    __asm__ volatile("rdtime %0" : "=r"(t));
    return t;
#else  /* x86-64 fallback */
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#endif
}

/* Simple decimal print (no libc) */
static void bench_print_u64(uint64_t v) {
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (v == 0) { microkit_dbg_puts("0"); return; }
    while (v > 0 && i > 0) {
        buf[--i] = '0' + (char)(v % 10);
        v /= 10;
    }
    microkit_dbg_puts(&buf[i]);
}

/* ── Single-publish benchmark ───────────────────────────────────────────── */

static uint64_t bench_single(void) {
    uint64_t t0 = bench_cycles();

    for (uint32_t i = 0; i < BENCH_MSG_COUNT; i++) {
        /* MR[1] = source PD id; label = event kind */
        microkit_mr_set(1, PD_ID_EVENT_BUS);
        microkit_ppcall(CH_BENCH_EVENTBUS,
                        microkit_msginfo_new(MSG_EVENT_AGENT_SPAWNED, 2));
    }

    return bench_cycles() - t0;
}

/* ── Batch-publish benchmark ────────────────────────────────────────────── */

static uint64_t bench_batch(void) {
    volatile agentos_batch_event_t *staging =
        (volatile agentos_batch_event_t *)
        ((uint8_t *)eventbus_ring_vaddr + EVENTBUS_BATCH_STAGING_OFFSET);

    uint64_t t0 = bench_cycles();

    uint32_t remaining = BENCH_MSG_COUNT;
    while (remaining > 0) {
        uint32_t batch_idx = 0;
        uint32_t n = remaining < EVENTBUS_BATCH_MAX ? remaining : EVENTBUS_BATCH_MAX;

        for (uint32_t i = 0; i < n; i++) {
            MSGBUS_BATCH_PUSH(staging, batch_idx,
                              MSG_EVENT_AGENT_SPAWNED, PD_ID_EVENT_BUS,
                              NULL, 0);
        }

        microkit_mr_set(0, 0);
        microkit_mr_set(1, batch_idx);
        microkit_ppcall(CH_BENCH_EVENTBUS,
                        microkit_msginfo_new(MSG_EVENTBUS_PUBLISH_BATCH, 2));

        remaining -= n;
    }

    return bench_cycles() - t0;
}

/* ── Microkit entry points ──────────────────────────────────────────────── */

void init(void) {
    microkit_dbg_puts("[bench] IPC batch throughput benchmark\n");
    microkit_dbg_puts("[bench] msgs=");
    bench_print_u64(BENCH_MSG_COUNT);
    microkit_dbg_puts("\n");

    /* --- single publish --- */
    uint64_t single_cycles = bench_single();
    uint64_t single_per_msg = single_cycles / BENCH_MSG_COUNT;

    microkit_dbg_puts("[bench] SINGLE  total_cycles=");
    bench_print_u64(single_cycles);
    microkit_dbg_puts("  per_msg=");
    bench_print_u64(single_per_msg);
    microkit_dbg_puts("\n");

    /* --- batch publish --- */
    uint64_t batch_cycles = bench_batch();
    uint64_t batch_per_msg = batch_cycles / BENCH_MSG_COUNT;

    microkit_dbg_puts("[bench] BATCH   total_cycles=");
    bench_print_u64(batch_cycles);
    microkit_dbg_puts("  per_msg=");
    bench_print_u64(batch_per_msg);
    microkit_dbg_puts("\n");

    /* --- comparison --- */
    microkit_dbg_puts("[bench] speedup ~");
    if (batch_per_msg > 0) {
        bench_print_u64(single_per_msg / batch_per_msg);
        microkit_dbg_puts("x (batch vs single PPC round-trip)\n");
    } else {
        microkit_dbg_puts("N/A\n");
    }

    microkit_dbg_puts("[bench] DONE\n");
}

void notified(microkit_channel ch) {
    /* bench agent receives EventBus notifications as a side-effect subscriber */
    (void)ch;
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg) {
    (void)ch;
    (void)msg;
    return microkit_msginfo_new(0, 0);
}
