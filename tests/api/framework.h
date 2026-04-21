/*
 * framework.h — agentOS API test framework
 *
 * Provides:
 *   - TAP-format output helpers (TAP_PLAN, TAP_OK, TAP_FAIL, TAP_DIAG)
 *   - Minimal mock IPC layer (mock MR registers + mock_ppcall)
 *   - Convenience assertion macros (ASSERT_EQ, ASSERT_NE, ASSERT_TRUE)
 *
 * Include this header once per test file, before any other includes.
 *
 * Tests run entirely on the host — no seL4 required.  The mock IPC layer
 * matches the ABI used by the real agentOS Microkit runtime so that test code
 * looks identical to production driver code.
 *
 * Usage:
 *   #include "framework.h"
 *
 *   static void test_something(void) {
 *       mock_mr_set(0, OP_FOO);
 *       mock_mr_set(1, 42);
 *       mock_dispatch(0, 0);          // call the service under test
 *       TAP_OK("foo returns zero");   // assert separately if needed
 *   }
 *
 *   int main(void) {
 *       TAP_PLAN(1);
 *       test_something();
 *       return tap_exit();
 *   }
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef AGENTOS_TEST_FRAMEWORK_H
#define AGENTOS_TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

/* ── TAP state ───────────────────────────────────────────────────────────── */

static int _tap_planned  = 0;   /* total tests declared by TAP_PLAN       */
static int _tap_run      = 0;   /* tests executed so far                   */
static int _tap_failed   = 0;   /* number of not-ok results                */
static int _tap_todo     = 0;   /* tests marked TODO (informational)       */

/* ── TAP output primitives ───────────────────────────────────────────────── */

/*
 * TAP_PLAN(n) — emit the TAP plan line.
 *
 * Must be the very first TAP output (before any TAP_OK / TAP_FAIL).
 * Declare the total number of tests you intend to run.
 */
#define TAP_PLAN(n) do {                                        \
    _tap_planned = (n);                                         \
    printf("TAP version 14\n1..%d\n", (n));                    \
} while (0)

/*
 * TAP_OK(name) — emit a passing test point.
 */
#define TAP_OK(name) do {                                       \
    _tap_run++;                                                  \
    printf("ok %d - %s\n", _tap_run, (name));                  \
} while (0)

/*
 * TAP_FAIL(name, reason) — emit a failing test point with a diagnostic.
 *
 * The diagnostic line (prefixed with "# ") is printed immediately after.
 */
#define TAP_FAIL(name, reason) do {                             \
    _tap_run++;                                                  \
    _tap_failed++;                                               \
    printf("not ok %d - %s\n", _tap_run, (name));              \
    printf("  # %s\n", (reason));                               \
} while (0)

/*
 * TAP_DIAG(msg) — emit a diagnostic (comment) line.
 *
 * Diagnostics are not test points and do not affect the pass/fail count.
 */
#define TAP_DIAG(msg) printf("# %s\n", (msg))

/*
 * TAP_TODO(name, reason) — emit a TODO test point (always passes the suite).
 *
 * Use for tests that describe planned but not-yet-implemented behaviour.
 */
#define TAP_TODO(name, reason) do {                             \
    _tap_run++;                                                  \
    _tap_todo++;                                                 \
    printf("ok %d - %s # TODO %s\n", _tap_run, (name), (reason)); \
} while (0)

/*
 * tap_exit() — finalise the run and return an exit code.
 *
 * Prints a summary line and returns 0 if all tests passed, 1 otherwise.
 * Call this as the last statement in main().
 */
static inline int tap_exit(void) {
    if (_tap_run != _tap_planned) {
        printf("# WARNING: planned %d tests but ran %d\n",
               _tap_planned, _tap_run);
    }
    if (_tap_failed == 0) {
        printf("# ALL %d TESTS PASSED\n", _tap_run);
        return 0;
    }
    printf("# FAILED %d/%d TESTS\n", _tap_failed, _tap_run);
    return 1;
}

/* ── Assertion helpers ───────────────────────────────────────────────────── */

/*
 * ASSERT_EQ(a, b, testname)
 *
 * Emit TAP_OK if (uint64_t)a == (uint64_t)b, TAP_FAIL otherwise.
 * The failure diagnostic prints both operand values in hex.
 */
#define ASSERT_EQ(a, b, testname) do {                          \
    uint64_t _a = (uint64_t)(a);                                \
    uint64_t _b = (uint64_t)(b);                                \
    if (_a == _b) {                                             \
        TAP_OK(testname);                                       \
    } else {                                                    \
        char _diag[128];                                        \
        snprintf(_diag, sizeof(_diag),                          \
                 "expected 0x%llx got 0x%llx",                  \
                 (unsigned long long)_b,                        \
                 (unsigned long long)_a);                       \
        TAP_FAIL(testname, _diag);                              \
    }                                                           \
} while (0)

/*
 * ASSERT_NE(a, b, testname)
 *
 * Emit TAP_OK if (uint64_t)a != (uint64_t)b, TAP_FAIL otherwise.
 */
#define ASSERT_NE(a, b, testname) do {                          \
    uint64_t _a = (uint64_t)(a);                                \
    uint64_t _b = (uint64_t)(b);                                \
    if (_a != _b) {                                             \
        TAP_OK(testname);                                       \
    } else {                                                    \
        char _diag[128];                                        \
        snprintf(_diag, sizeof(_diag),                          \
                 "expected values to differ but both are 0x%llx", \
                 (unsigned long long)_a);                       \
        TAP_FAIL(testname, _diag);                              \
    }                                                           \
} while (0)

/*
 * ASSERT_TRUE(expr, testname)
 *
 * Emit TAP_OK if expr is non-zero, TAP_FAIL otherwise.
 */
#define ASSERT_TRUE(expr, testname) do {                        \
    if (!!(expr)) {                                             \
        TAP_OK(testname);                                       \
    } else {                                                    \
        TAP_FAIL(testname, "expression was false");             \
    }                                                           \
} while (0)

/*
 * ASSERT_FALSE(expr, testname)
 *
 * Emit TAP_OK if expr is zero, TAP_FAIL otherwise.
 */
#define ASSERT_FALSE(expr, testname) do {                       \
    if (!(expr)) {                                              \
        TAP_OK(testname);                                       \
    } else {                                                    \
        TAP_FAIL(testname, "expression was true");              \
    }                                                           \
} while (0)

/* ── Mock IPC message-register layer ─────────────────────────────────────── */

/*
 * agentOS Microkit ABI: agents communicate with services by setting MRs,
 * calling microkit_ppcall(channel, label, mr_count), and reading reply MRs.
 *
 * The mock layer below provides:
 *   mock_mr_set(i, v)   — write MR i
 *   mock_mr_get(i)      — read MR i
 *   mock_mr_clear()     — zero all MRs (call before each test invocation)
 *
 * Tests that exercise service implementations directly (by pulling in the
 * implementation source) drive the service via the same _mrs[] array and
 * the stub microkit_mr_set / microkit_mr_get functions defined below.
 *
 * Tests that model the *caller* side use mock_ipc_call() to record what the
 * caller would have sent and what reply it expects.
 */

#define MOCK_MR_COUNT 16u

static uint64_t _mrs[MOCK_MR_COUNT];

/* Aliases used by service implementations that call microkit_mr_* directly */
static inline void     microkit_mr_set(uint32_t i, uint64_t v) {
    if (i < MOCK_MR_COUNT) _mrs[i] = v;
}
static inline uint64_t microkit_mr_get(uint32_t i) {
    return (i < MOCK_MR_COUNT) ? _mrs[i] : 0;
}
static inline void     mock_mr_set(uint32_t i, uint64_t v) { microkit_mr_set(i, v); }
static inline uint64_t mock_mr_get(uint32_t i)             { return microkit_mr_get(i); }
static inline void     mock_mr_clear(void) {
    for (uint32_t i = 0; i < MOCK_MR_COUNT; i++) _mrs[i] = 0;
}

/*
 * mock_msginfo_new(label, count) — construct a packed MsgInfo value.
 *
 * In the real Microkit ABI, MsgInfo packs the label into the upper bits and
 * the MR count into the lower bits.  Tests that exercise the dispatch path
 * only need the label; the count is informational and many service stubs
 * ignore it.
 */
static inline uint64_t mock_msginfo_new(uint64_t label, uint32_t count) {
    (void)count;
    return label;
}

/*
 * Microkit type/function stubs for service implementations that reference them
 * at compile time but are not exercised in host-side tests.
 */
typedef uint32_t microkit_channel;
typedef uint64_t microkit_msginfo;

static inline microkit_msginfo microkit_msginfo_new(uint64_t label, uint32_t count) {
    return mock_msginfo_new(label, count);
}
static inline void microkit_dbg_puts(const char *s) { (void)s; }
static inline void microkit_notify(microkit_channel ch) { (void)ch; }

/*
 * mock_ipc_call — record a caller-side IPC invocation for assertion.
 *
 * Writes MR[0]=label + MR[1..n]=args, calls the provided dispatch function,
 * then returns MR[0] (the service's status reply).
 *
 * dispatch_fn matches the signature used by every service under test:
 *   void dispatch(microkit_channel ch, microkit_msginfo info)
 *
 * Usage:
 *   uint64_t status = mock_ipc_call(CHAN_MSGBUS, OP_SUBSCRIBE,
 *                                   dispatch_fn, 1, topic_id);
 */
typedef void (*dispatch_fn_t)(microkit_channel, microkit_msginfo);

static inline uint64_t mock_ipc_call(
    microkit_channel channel,
    uint64_t         label,
    dispatch_fn_t    dispatch,
    uint32_t         n_args,
    ...)
{
    mock_mr_clear();
    _mrs[0] = label;

    va_list ap;
    va_start(ap, n_args);
    for (uint32_t i = 0; i < n_args && (i + 1) < MOCK_MR_COUNT; i++) {
        _mrs[i + 1] = va_arg(ap, uint64_t);
    }
    va_end(ap);

    dispatch(channel, mock_msginfo_new(label, n_args + 1));
    return _mrs[0];  /* status in MR0 */
}

/* ── Common error codes (mirror contracts/ when they exist) ──────────────── */

/*
 * Standard agentOS IPC status codes placed in MR[0] by every service.
 * TODO: replace with #include "../../contracts/<service>/interface.h"
 *       once those headers are written.
 */
#define AOS_OK              0x00u  /* operation succeeded                  */
#define AOS_ERR_GENERIC     0xFFu  /* catch-all failure                    */
#define AOS_ERR_NOT_FOUND   0x01u  /* object / subscriber not found        */
#define AOS_ERR_EXISTS      0x02u  /* object already exists                */
#define AOS_ERR_PERM        0x03u  /* capability / permission denied       */
#define AOS_ERR_INVAL       0x04u  /* invalid argument                     */
#define AOS_ERR_NOSPC       0x05u  /* table / buffer full                  */
#define AOS_ERR_TIMEOUT     0x06u  /* operation timed out                  */
#define AOS_ERR_BUSY        0x07u  /* resource is busy                     */
#define AOS_ERR_TOO_LARGE   0x08u  /* payload / buffer too large           */
#define AOS_ERR_UNIMPL      0x09u  /* opcode not implemented               */

#endif /* AGENTOS_TEST_FRAMEWORK_H */
