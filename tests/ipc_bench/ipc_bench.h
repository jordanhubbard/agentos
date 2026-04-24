/*
 * ipc_bench.h — agentOS seL4 IPC latency/throughput benchmark types and constants
 *
 * Shared between ipc_bench.c and any future harness that wants to interpret
 * the TAP output or baseline file.
 *
 * In AGENTOS_TEST_HOST mode the timing source is clock_gettime(CLOCK_MONOTONIC)
 * (nanosecond precision).  In bare-metal AArch64 mode it is the virtual counter
 * register cntvct_el0 (tick = 1/CNTFRQ_EL0; QEMU TCG runs at 62.5 MHz by
 * default but we normalise to 1 GHz reference for reporting).
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

/* ── Build-mode guard ────────────────────────────────────────────────────── */
/*
 * AGENTOS_TEST_HOST:
 *   Defined when building on a POSIX host (macOS, Linux) for development and
 *   CI.  In this mode seL4 IPC is simulated via direct function calls so the
 *   benchmark measures host-OS call overhead instead of real seL4 overhead.
 *   All TAP output is still produced and the pass/fail thresholds are the same.
 *
 * Without AGENTOS_TEST_HOST:
 *   Bare-metal AArch64 build.  Requires the seL4 microkit headers and the
 *   agentOS sel4_ipc.h / sel4_boot.h wrappers.
 */

/* ── Benchmark parameters ────────────────────────────────────────────────── */

/* Number of iterations for ping-pong and notify latency measurements. */
#define BENCH_PINGPONG_ITERS    10000u

/* Number of iterations for notify latency measurements. */
#define BENCH_NOTIFY_ITERS      10000u

/* Number of iterations per simulated client for throughput measurement. */
#define BENCH_THROUGHPUT_ITERS  1000u

/* Number of simulated concurrent clients for throughput measurement. */
#define BENCH_THROUGHPUT_CLIENTS 4u

/* Number of iterations for cap-transfer overhead measurement. */
#define BENCH_CAP_ITERS         1000u

/* ── Pass/fail thresholds ────────────────────────────────────────────────── */
/*
 * All thresholds are deliberately conservative: they must pass in QEMU TCG
 * emulation (slow) as well as on real hardware (fast).  They exist to catch
 * regressions (e.g. accidental busy-loops, extra kernel crossings) not to
 * enforce tight real-time budgets.
 */

/* BENCH_PINGPONG: mean round-trip latency must be under this (nanoseconds). */
#define BENCH_PINGPONG_THRESHOLD_NS   300000ULL    /* 300 µs */

/* BENCH_NOTIFY: mean signal latency must be under this (nanoseconds). */
#define BENCH_NOTIFY_THRESHOLD_NS     500000ULL    /* 500 µs */

/* BENCH_THROUGHPUT: ops/sec must exceed this. */
#define BENCH_THROUGHPUT_THRESHOLD    1000ULL      /* 1000 ops/sec */

/* ── Reference clock ─────────────────────────────────────────────────────── */
/*
 * In bare-metal mode we convert counter ticks to nanoseconds using:
 *   ns = ticks * 1,000,000,000 / BENCH_REF_HZ
 *
 * QEMU TCG reports CNTFRQ_EL0 = 62500000 (62.5 MHz) but using 1 GHz as a
 * "reference" makes the reported numbers comparable to wall-clock estimates
 * and avoids a divide at runtime.  Set this to the actual CNTFRQ_EL0 value
 * for real-hardware runs.
 */
#define BENCH_REF_HZ   1000000000ULL  /* 1 GHz reference */

/* ── Statistics ─────────────────────────────────────────────────────────── */

#define BENCH_MAX_SAMPLES  10000u

/*
 * bench_stat_t — accumulates timing samples for post-run analysis.
 *
 * Integer-only; no floating point.  p99 is computed via partial sort.
 */
typedef struct {
    uint64_t min;
    uint64_t max;
    uint64_t sum;
    uint64_t samples[BENCH_MAX_SAMPLES];
    uint32_t count;
} bench_stat_t;

/* ── IPC stub opcode (used in simulated server in test-host mode) ─────────── */

#define BENCH_OP_PING    0x4201u
#define BENCH_OP_NOTIFY  0x4202u

/* ── TAP test count ──────────────────────────────────────────────────────── */

#define BENCH_TAP_TOTAL  4u
