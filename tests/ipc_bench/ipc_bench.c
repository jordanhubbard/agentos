/*
 * ipc_bench.c — agentOS seL4 IPC latency/throughput benchmark
 *
 * Measures four aspects of raw seL4 IPC performance using the sel4_ipc.h API:
 *
 *   1. BENCH_PINGPONG   — synchronous call/reply round-trip latency
 *   2. BENCH_NOTIFY     — notification signal latency (fire-and-forget)
 *   3. BENCH_THROUGHPUT — bulk operation throughput (ops/sec) with multiple
 *                         simulated clients
 *   4. BENCH_CAP_TRANSFER — overhead of including a capability in a message
 *                           vs a bare data message
 *
 * The benchmark doubles as a correctness test: if call/reply ordering is
 * wrong the server loop deadlocks and the process never produces TAP output.
 *
 * Output format: TAP version 14 (machine-readable, consumed by xtask test).
 *
 * Build modes:
 *   AGENTOS_TEST_HOST   — host (macOS / Linux) build; seL4 IPC replaced by
 *                         direct function calls; timing via clock_gettime.
 *   (no define)         — bare-metal AArch64 seL4 build; timing via
 *                         cntvct_el0 virtual counter.
 *
 * Build (host):
 *   cc -std=c11 -Wall -Wextra -DAGENTOS_TEST_HOST -o ipc_bench_host \
 *       tests/ipc_bench/ipc_bench.c -lm
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ipc_bench.h"

/* ── Platform abstraction ─────────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST
/*
 * Host build: pull in POSIX headers for clock_gettime and printf.
 * No seL4 or Microkit headers needed.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/*
 * bench_read_cycle() — return a monotonic timestamp in nanoseconds.
 *
 * Used as a "cycle" counter in host mode; the value is already in ns so
 * bench_cycles_to_ns() is a no-op identity transform below.
 */
static inline uint64_t bench_read_cycle(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * In host mode cycles are already nanoseconds; no conversion needed.
 */
static inline uint64_t bench_cycles_to_ns(uint64_t cycles)
{
    return cycles;
}

/* Output helpers — use standard putchar/printf on the host. */
static void bench_putchar(char c) { putchar(c); }

#else /* bare-metal AArch64 */

/*
 * Bare-metal build: no libc.  Include the seL4 IPC wrappers.
 */
#include "kernel/agentos-root-task/include/sel4_ipc.h"
#include "kernel/agentos-root-task/include/sel4_boot.h"

/*
 * bench_read_cycle() — read the AArch64 virtual counter.
 *
 * cntvct_el0 is accessible from EL0 (user mode) without special privileges
 * (CNTKCTL_EL1.EL0VCTEN must be set by the kernel, which seL4 does).
 * The counter increments at CNTFRQ_EL0 Hz; see BENCH_REF_HZ for the
 * normalisation constant used when converting to nanoseconds.
 */
static inline uint64_t bench_read_cycle(void)
{
    uint64_t cnt;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt));
    return cnt;
}

/*
 * bench_cycles_to_ns() — convert counter ticks to nanoseconds.
 *
 * Uses BENCH_REF_HZ (1 GHz reference).  For QEMU TCG this over-reports
 * actual time (CNTFRQ = 62.5 MHz) but gives stable, comparable numbers.
 * For real hardware set BENCH_REF_HZ to the actual CNTFRQ_EL0 value.
 */
static inline uint64_t bench_cycles_to_ns(uint64_t cycles)
{
    /* ns = cycles * 1,000,000,000 / BENCH_REF_HZ
     * With BENCH_REF_HZ == 1e9 this is the identity; kept for documentation. */
    return cycles * 1000000000ULL / BENCH_REF_HZ;
}

/*
 * Output helpers — use seL4_DebugPutChar in bare-metal mode.
 * seL4_DebugPutChar is always present in debug kernel builds.
 */
static void bench_putchar(char c)
{
    /* seL4 debug syscall: DebugPutChar is invocation 1 on ARM. */
    register uint64_t a0 __asm__("x0") = (uint64_t)(unsigned char)c;
    register uint64_t sc __asm__("x7") = (uint64_t)(unsigned)-12; /* seL4_SysDebugPutChar */
    __asm__ volatile("svc #0" : : "r"(a0), "r"(sc) : "memory");
}

#endif /* AGENTOS_TEST_HOST */

/* ── Output primitives (no libc printf in bare-metal) ─────────────────────── */

static void bench_puts(const char *s)
{
    while (*s)
        bench_putchar(*s++);
}

static void bench_put_uint(uint64_t v)
{
    char buf[21]; /* log10(2^64) < 20 */
    int  i = 20;
    buf[20] = '\0';
    if (v == 0) {
        bench_putchar('0');
        return;
    }
    while (v > 0 && i > 0) {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    bench_puts(buf + i);
}

static void bench_put_int(int64_t v)
{
    if (v < 0) {
        bench_putchar('-');
        bench_put_uint((uint64_t)(-v));
    } else {
        bench_put_uint((uint64_t)v);
    }
}

/* ── Statistics ─────────────────────────────────────────────────────────── */

static void bench_stat_init(bench_stat_t *s)
{
    s->min   = UINT64_MAX;
    s->max   = 0;
    s->sum   = 0;
    s->count = 0;
}

static void bench_stat_record(bench_stat_t *s, uint64_t val)
{
    if (s->count >= BENCH_MAX_SAMPLES)
        return;
    s->samples[s->count++] = val;
    s->sum += val;
    if (val < s->min) s->min = val;
    if (val > s->max) s->max = val;
}

static uint64_t bench_stat_mean(const bench_stat_t *s)
{
    if (s->count == 0) return 0;
    return s->sum / (uint64_t)s->count;
}

/*
 * bench_stat_p99() — return the 99th-percentile sample value.
 *
 * Uses a partial insertion sort up to the target index to avoid sorting the
 * entire 10,000-element array.  O(n * (n - target)) in the worst case, but
 * for p99 of 10,000 samples only 100 full passes are needed — acceptable.
 *
 * Sorts samples[] in place (ascending).
 */
static uint64_t bench_stat_p99(bench_stat_t *s)
{
    if (s->count == 0) return 0;

    uint32_t target = (uint32_t)(((uint64_t)s->count * 99u) / 100u);
    if (target >= s->count) target = s->count - 1u;

    /* Partial selection sort: find the (target+1) smallest values. */
    uint32_t sorted = 0;
    while (sorted <= target) {
        uint32_t min_idx = sorted;
        for (uint32_t j = sorted + 1u; j < s->count; j++) {
            if (s->samples[j] < s->samples[min_idx])
                min_idx = j;
        }
        /* Swap samples[sorted] and samples[min_idx]. */
        uint64_t tmp          = s->samples[sorted];
        s->samples[sorted]    = s->samples[min_idx];
        s->samples[min_idx]   = tmp;
        sorted++;
    }
    return s->samples[target];
}

/* ── TAP output helpers ──────────────────────────────────────────────────── */

static uint32_t g_tap_next = 1u;  /* next TAP test number */
static uint32_t g_tap_failures = 0u;

static void tap_header(void)
{
    bench_puts("TAP version 14\n1..");
    bench_put_uint(BENCH_TAP_TOTAL);
    bench_putchar('\n');
}

/* ── Simulated seL4 IPC for AGENTOS_TEST_HOST mode ──────────────────────── */
/*
 * In the host build we have no seL4 kernel.  We simulate the round-trip by
 * calling a "server" function directly.  This measures pure function-call
 * overhead (no kernel crossing) and validates the benchmark logic itself.
 *
 * The indirection through a volatile function pointer prevents the compiler
 * from inlining the callee and removing the call entirely.
 */

#ifdef AGENTOS_TEST_HOST

/*
 * Simulated message struct — mirrors sel4_msg_t layout so the same benchmark
 * code compiles in both modes.
 */
typedef struct {
    uint32_t opcode;
    uint32_t length;
    uint8_t  data[48];
} sim_msg_t;

/* Simulated server: receives req, writes an identical echo reply. */
static void sim_server_handle(const sim_msg_t *req, sim_msg_t *rep)
{
    rep->opcode = 0u;               /* SEL4_ERR_OK */
    rep->length = req->length;
    /* Echo first 8 bytes of payload to validate correctness. */
    uint32_t copy = req->length < 8u ? req->length : 8u;
    for (uint32_t i = 0; i < copy; i++)
        rep->data[i] = req->data[i];
}

typedef void (*sim_handler_fn)(const sim_msg_t *, sim_msg_t *);
static volatile sim_handler_fn g_sim_handler = sim_server_handle;

/*
 * sim_call() — simulate one seL4_Call round-trip.
 *
 * Calls the server handler directly (no kernel crossing).  The volatile
 * function pointer ensures the call is not eliminated by the optimiser.
 */
static inline void sim_call(const sim_msg_t *req, sim_msg_t *rep)
{
    g_sim_handler(req, rep);
}

/*
 * sim_notify() — simulate one seL4_Signal (fire-and-forget notification).
 *
 * In seL4 this is a one-way send with no reply.  On the host we model it as
 * calling a no-op handler and returning immediately — the latency measured is
 * the call overhead without the reply path.
 */
static volatile uint64_t g_notify_badge_sink;  /* prevent optimisation */

static inline void sim_notify(uint64_t badge)
{
    g_notify_badge_sink = badge;
    /* Memory barrier: prevents the store from being hoisted above the
     * surrounding timing calls. */
    __asm__ volatile("" ::: "memory");
}

/* seL4_MessageInfo_new is used for BENCH_CAP_TRANSFER; provide a stub. */
#define seL4_MessageInfo_new(label, cu, ec, len) \
    ((uint64_t)(((label) << 12) | ((ec) << 7) | (len)))

/* Simulated cap-transfer call — just calls the normal handler. */
static inline void sim_call_with_cap(const sim_msg_t *req, sim_msg_t *rep,
                                     uint64_t msginfo)
{
    (void)msginfo;
    g_sim_handler(req, rep);
}

#endif /* AGENTOS_TEST_HOST */

/* ══════════════════════════════════════════════════════════════════════════
 * BENCH_PINGPONG — synchronous round-trip latency
 * ══════════════════════════════════════════════════════════════════════════ */

static void bench_pingpong(void)
{
    bench_stat_t stat;
    bench_stat_init(&stat);

#ifdef AGENTOS_TEST_HOST
    sim_msg_t req, rep;
    req.opcode = BENCH_OP_PING;
    req.length = 4u;
    req.data[0] = 0xABu;
    req.data[1] = 0xCDu;
    req.data[2] = 0xEFu;
    req.data[3] = 0x01u;

    for (uint32_t i = 0; i < BENCH_PINGPONG_ITERS; i++) {
        uint64_t t0 = bench_read_cycle();
        sim_call(&req, &rep);
        uint64_t t1 = bench_read_cycle();
        uint64_t delta = bench_cycles_to_ns(t1 - t0);
        bench_stat_record(&stat, delta);
        /* Correctness check: reply must echo our opcode status (0 = OK). */
        if (rep.opcode != 0u) {
            bench_puts("not ok ");
            bench_put_uint(g_tap_next);
            bench_puts(" - BENCH_PINGPONG: server replied with error opcode ");
            bench_put_uint(rep.opcode);
            bench_putchar('\n');
            g_tap_next++;
            g_tap_failures++;
            return;
        }
    }
#else
    /* Bare-metal: use the real sel4_ipc.h sel4_call wrapper.
     * The endpoint capability (BENCH_PINGPONG_EP) must be provisioned by the
     * root task and placed in the well-known slot defined below. */
    #define BENCH_PINGPONG_EP  ((seL4_CPtr)20u)  /* root task assigns slot 20 */

    sel4_msg_t req, rep;
    req.opcode = BENCH_OP_PING;
    req.length = 4u;
    req.data[0] = 0xABu;
    req.data[1] = 0xCDu;
    req.data[2] = 0xEFu;
    req.data[3] = 0x01u;

    for (uint32_t i = 0; i < BENCH_PINGPONG_ITERS; i++) {
        uint64_t t0 = bench_read_cycle();
        sel4_call(BENCH_PINGPONG_EP, &req, &rep);
        uint64_t t1 = bench_read_cycle();
        uint64_t delta = bench_cycles_to_ns(t1 - t0);
        bench_stat_record(&stat, delta);
    }
#endif /* AGENTOS_TEST_HOST */

    uint64_t mean = bench_stat_mean(&stat);
    uint64_t p99  = bench_stat_p99(&stat);
    int      pass = (mean < BENCH_PINGPONG_THRESHOLD_NS);

    /* Emit TAP line with detailed diagnostics. */
    if (pass) bench_puts("ok "); else bench_puts("not ok ");
    bench_put_uint(g_tap_next++);
    bench_puts(" - BENCH_PINGPONG: mean=");
    bench_put_uint(mean);
    bench_puts("ns min=");
    bench_put_uint(stat.min);
    bench_puts("ns max=");
    bench_put_uint(stat.max);
    bench_puts("ns p99=");
    bench_put_uint(p99);
    bench_puts("ns\n");

    if (!pass) g_tap_failures++;
}

/* ══════════════════════════════════════════════════════════════════════════
 * BENCH_NOTIFY — notification signal latency
 * ══════════════════════════════════════════════════════════════════════════ */

static void bench_notify(void)
{
    bench_stat_t stat;
    bench_stat_init(&stat);

#ifdef AGENTOS_TEST_HOST
    for (uint32_t i = 0; i < BENCH_NOTIFY_ITERS; i++) {
        uint64_t t0 = bench_read_cycle();
        sim_notify((uint64_t)(i + 1u));
        uint64_t t1 = bench_read_cycle();
        bench_stat_record(&stat, bench_cycles_to_ns(t1 - t0));
    }
#else
    /* Bare-metal: use seL4_Signal on a notification object.
     * Slot 21 is reserved for the benchmark notification cap. */
    #define BENCH_NOTIFY_EP  ((seL4_CPtr)21u)

    for (uint32_t i = 0; i < BENCH_NOTIFY_ITERS; i++) {
        uint64_t t0 = bench_read_cycle();
        /* seL4_Signal is equivalent to seL4_Send on a notification object. */
        seL4_Word badge = (seL4_Word)(i + 1u);
        seL4_MessageInfo_t mi = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, badge);
        seL4_Send(BENCH_NOTIFY_EP, mi);
        uint64_t t1 = bench_read_cycle();
        bench_stat_record(&stat, bench_cycles_to_ns(t1 - t0));
    }
#endif

    uint64_t mean = bench_stat_mean(&stat);
    int      pass = (mean < BENCH_NOTIFY_THRESHOLD_NS);

    if (pass) bench_puts("ok "); else bench_puts("not ok ");
    bench_put_uint(g_tap_next++);
    bench_puts(" - BENCH_NOTIFY: mean=");
    bench_put_uint(mean);
    bench_puts("ns\n");

    if (!pass) g_tap_failures++;
}

/* ══════════════════════════════════════════════════════════════════════════
 * BENCH_THROUGHPUT — bulk operations per second
 * ══════════════════════════════════════════════════════════════════════════ */

static void bench_throughput(void)
{
    /*
     * Simulate BENCH_THROUGHPUT_CLIENTS independent "client" call sequences,
     * each performing BENCH_THROUGHPUT_ITERS calls.
     *
     * Total calls = BENCH_THROUGHPUT_CLIENTS * BENCH_THROUGHPUT_ITERS.
     * We measure total wall time and compute ops/sec.
     *
     * In the host build this is a simple nested loop over sim_call().
     * In bare-metal mode each client uses the same endpoint (the server must
     * be prepared to handle round-robin callers).
     */
    uint64_t total_calls = (uint64_t)BENCH_THROUGHPUT_CLIENTS *
                           (uint64_t)BENCH_THROUGHPUT_ITERS;

#ifdef AGENTOS_TEST_HOST
    sim_msg_t req, rep;
    req.opcode = BENCH_OP_PING;
    req.length = 0u;

    uint64_t t0 = bench_read_cycle();

    for (uint32_t c = 0; c < BENCH_THROUGHPUT_CLIENTS; c++) {
        for (uint32_t i = 0; i < BENCH_THROUGHPUT_ITERS; i++) {
            req.data[0] = (uint8_t)c;
            req.data[1] = (uint8_t)(i & 0xFFu);
            sim_call(&req, &rep);
        }
    }

    uint64_t t1  = bench_read_cycle();
    uint64_t elapsed_ns = bench_cycles_to_ns(t1 - t0);
#else
    sel4_msg_t req, rep;
    req.opcode = BENCH_OP_PING;
    req.length = 0u;

    uint64_t t0 = bench_read_cycle();

    for (uint32_t c = 0; c < BENCH_THROUGHPUT_CLIENTS; c++) {
        for (uint32_t i = 0; i < BENCH_THROUGHPUT_ITERS; i++) {
            req.data[0] = (uint8_t)c;
            req.data[1] = (uint8_t)(i & 0xFFu);
            sel4_call(BENCH_PINGPONG_EP, &req, &rep);
        }
    }

    uint64_t t1  = bench_read_cycle();
    uint64_t elapsed_ns = bench_cycles_to_ns(t1 - t0);
#endif

    /*
     * ops_per_sec = total_calls * 1e9 / elapsed_ns
     *
     * Computed in integer arithmetic:
     *   ops_per_sec = (total_calls * 1,000,000,000) / elapsed_ns
     *
     * To avoid 64-bit overflow with 4000 * 1e9 = 4e12 (fits in uint64_t):
     *   max representable in uint64_t ≈ 1.8e19 — we are safe.
     */
    uint64_t ops_per_sec = 0;
    if (elapsed_ns > 0)
        ops_per_sec = (total_calls * 1000000000ULL) / elapsed_ns;

    int pass = (ops_per_sec >= BENCH_THROUGHPUT_THRESHOLD);

    if (pass) bench_puts("ok "); else bench_puts("not ok ");
    bench_put_uint(g_tap_next++);
    bench_puts(" - BENCH_THROUGHPUT: ");
    bench_put_uint(ops_per_sec);
    bench_puts(" ops/sec\n");

    if (!pass) g_tap_failures++;
}

/* ══════════════════════════════════════════════════════════════════════════
 * BENCH_CAP_TRANSFER — capability-transfer overhead
 * ══════════════════════════════════════════════════════════════════════════ */

static void bench_cap_transfer(void)
{
    /*
     * Measure the per-call overhead of including a capability in the message
     * (extraCaps=1) vs a plain data call (extraCaps=0).
     *
     * Both measurements use the same number of iterations; the difference
     * (which may be negative due to measurement noise) is reported in
     * "cycles per call" (raw counter ticks, not nanoseconds).
     *
     * This test always emits "ok" — it is informational.
     */
    uint64_t t_nocap_start, t_nocap_end;
    uint64_t t_cap_start,   t_cap_end;

#ifdef AGENTOS_TEST_HOST
    sim_msg_t req, rep;
    req.opcode = BENCH_OP_PING;
    req.length = 8u;

    /* Without cap */
    t_nocap_start = bench_read_cycle();
    for (uint32_t i = 0; i < BENCH_CAP_ITERS; i++) {
        sim_call(&req, &rep);
    }
    t_nocap_end = bench_read_cycle();

    /* With cap (simulated: same call but with a seL4_MessageInfo_new that
     * sets extraCaps=1 — the sim handler ignores it, measuring only the
     * overhead of constructing the message info word). */
    t_cap_start = bench_read_cycle();
    for (uint32_t i = 0; i < BENCH_CAP_ITERS; i++) {
        uint64_t mi = seL4_MessageInfo_new(BENCH_OP_PING, 0u, 1u, 2u);
        (void)mi;
        sim_call_with_cap(&req, &rep, mi);
    }
    t_cap_end = bench_read_cycle();

#else /* bare-metal */
    sel4_msg_t req, rep;
    req.opcode = BENCH_OP_PING;
    req.length = 8u;

    t_nocap_start = bench_read_cycle();
    for (uint32_t i = 0; i < BENCH_CAP_ITERS; i++) {
        sel4_call(BENCH_PINGPONG_EP, &req, &rep);
    }
    t_nocap_end = bench_read_cycle();

    t_cap_start = bench_read_cycle();
    for (uint32_t i = 0; i < BENCH_CAP_ITERS; i++) {
        /* Load a dummy cap into the extra-cap slot and call with extraCaps=1. */
        seL4_GetIPCBuffer()->caps_or_badges[0] = (seL4_Word)seL4_CapNull;
        _sel4_msg_to_mrs(&req);
        seL4_MessageInfo_t mi = seL4_MessageInfo_new(
            (uint64_t)req.opcode, 0u, 1u, (uint64_t)_SEL4_MR_COUNT);
        seL4_MessageInfo_t ri = seL4_Call(BENCH_PINGPONG_EP, mi);
        (void)ri;
        _sel4_mrs_to_msg(&rep);
    }
    t_cap_end = bench_read_cycle();

#endif /* AGENTOS_TEST_HOST */

    uint64_t nocap_ticks = t_nocap_end - t_nocap_start;
    uint64_t cap_ticks   = t_cap_end   - t_cap_start;

    /* Per-call overhead in raw ticks (signed — may be negative due to noise). */
    int64_t overhead_per_call = 0;
    if (BENCH_CAP_ITERS > 0) {
        int64_t cap_total   = (int64_t)cap_ticks;
        int64_t nocap_total = (int64_t)nocap_ticks;
        overhead_per_call   = (cap_total - nocap_total) / (int64_t)BENCH_CAP_ITERS;
    }

    /* Always passes — this is informational. */
    bench_puts("ok ");
    bench_put_uint(g_tap_next++);
    bench_puts(" - BENCH_CAP_TRANSFER: cap_overhead=");
    if (overhead_per_call >= 0) bench_putchar('+');
    bench_put_int(overhead_per_call);
    bench_puts(" cycles/call\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Entry point
 * ══════════════════════════════════════════════════════════════════════════ */

#ifdef AGENTOS_TEST_HOST

int main(void)
{
    tap_header();
    bench_pingpong();
    bench_notify();
    bench_throughput();
    bench_cap_transfer();

    /* TAP_DONE:<exit-code> — consumed by xtask test harness. */
    bench_puts("TAP_DONE:");
    bench_put_uint((uint64_t)g_tap_failures);
    bench_putchar('\n');

    return (int)(g_tap_failures > 0u ? 1 : 0);
}

#else /* bare-metal AArch64 */

/*
 * pd_main() — protection domain entry point (replaces main() in seL4 builds).
 *
 * The root task must:
 *   1. Retype an untyped as an endpoint and place it in cap slot 20
 *      (BENCH_PINGPONG_EP).
 *   2. Retype a notification object and place it in cap slot 21
 *      (BENCH_NOTIFY_EP).
 *   3. Create a server PD that listens on endpoint 20 and replies to every
 *      BENCH_OP_PING message it receives.
 *   4. Start this PD (the caller) after the server is ready.
 *
 * The server PD is a minimal sel4_server_run() loop — see the benchmark
 * README for the companion server source.
 */
void pd_main(void)
{
    tap_header();
    bench_pingpong();
    bench_notify();
    bench_throughput();
    bench_cap_transfer();

    bench_puts("TAP_DONE:");
    bench_put_uint((uint64_t)g_tap_failures);
    bench_putchar('\n');

    /* Suspend self — do not exit (no OS underneath). */
    seL4_TCB_Suspend(seL4_CapInitThreadTCB);
    /* Unreachable. */
    while (1) { seL4_Yield(); }
}

#endif /* AGENTOS_TEST_HOST */
