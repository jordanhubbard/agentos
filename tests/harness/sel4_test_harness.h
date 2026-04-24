/*
 * sel4_test_harness.h — host-side TAP test harness for VOS lifecycle tests
 *
 * Provides SEL4_TEST_BEGIN(), SEL4_ASSERT_EQ(), SEL4_ASSERT_NE(),
 * SEL4_ASSERT_OK(), and SEL4_ASSERT_TRUE() macros that produce TAP output
 * via stdio.
 *
 * Designed for use under AGENTOS_TEST_HOST: no seL4 / Microkit dependencies.
 * Compile with -DAGENTOS_TEST_HOST and link against libc.
 *
 * Usage:
 *   static void test_foo(void) {
 *       SEL4_TEST_BEGIN("foo");
 *       vos_handle_t h;
 *       vos_err_t rc = vos_create(&spec, &h);
 *       SEL4_ASSERT_OK(rc, "create returns ok");
 *   }
 *
 * TAP output:
 *   TAP version 14
 *   # --- TEST: foo ---
 *   ok 1 - create returns ok
 *   ...
 *   1..N
 *   # passed: P
 *   # failed: F
 *   TAP_DONE:<exit_code>
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ── Global counters ─────────────────────────────────────────────────────────── */

static int _sel4_tap_total  = 0;
static int _sel4_tap_passed = 0;
static int _sel4_tap_failed = 0;

/* ── TAP lifecycle ───────────────────────────────────────────────────────────── */

/*
 * SEL4_TAP_INIT(suite) — emit TAP header.  Call once before any tests.
 */
#define SEL4_TAP_INIT(suite) \
    do { \
        printf("TAP version 14\n"); \
        printf("# suite: %s\n", (suite)); \
    } while (0)

/*
 * SEL4_TAP_FINISH() — emit plan line and summary.  Call once at end.
 */
#define SEL4_TAP_FINISH() \
    do { \
        printf("1..%d\n", _sel4_tap_total); \
        printf("# passed: %d\n", _sel4_tap_passed); \
        printf("# failed: %d\n", _sel4_tap_failed); \
        printf("TAP_DONE:%d\n", (_sel4_tap_failed > 0) ? 1 : 0); \
    } while (0)

/*
 * SEL4_TAP_EXIT_CODE() — returns 0 if all tests passed, 1 otherwise.
 */
#define SEL4_TAP_EXIT_CODE() ((_sel4_tap_failed > 0) ? 1 : 0)

/* ── Test section marker ─────────────────────────────────────────────────────── */

/*
 * SEL4_TEST_BEGIN(name) — emit a TAP diagnostic marking the start of a
 * named test.  Does not count as a test point.
 * Use at the top of each static test function.
 */
#define SEL4_TEST_BEGIN(name) \
    printf("# --- TEST: %s ---\n", (name))

/* Alias for callers that use the SEL4_TEST name */
#define SEL4_TEST(name) SEL4_TEST_BEGIN(name)

/* ── Internal emitters ───────────────────────────────────────────────────────── */

#define _SEL4_OK(name) \
    do { \
        _sel4_tap_total++; \
        _sel4_tap_passed++; \
        printf("ok %d - %s\n", _sel4_tap_total, (name)); \
    } while (0)

#define _SEL4_FAIL(name, diag) \
    do { \
        _sel4_tap_total++; \
        _sel4_tap_failed++; \
        printf("not ok %d - %s\n", _sel4_tap_total, (name)); \
        printf("  # %s\n", (diag)); \
    } while (0)

/* ── Assertion macros ────────────────────────────────────────────────────────── */

/*
 * SEL4_ASSERT_EQ(a, b, name) — pass if (int64_t)(a) == (int64_t)(b)
 */
#define SEL4_ASSERT_EQ(a, b, name) \
    do { \
        int64_t _a = (int64_t)(a); \
        int64_t _b = (int64_t)(b); \
        if (_a == _b) { \
            _SEL4_OK(name); \
        } else { \
            char _buf[128]; \
            snprintf(_buf, sizeof(_buf), "got %lld expected %lld", \
                     (long long)_a, (long long)_b); \
            _SEL4_FAIL(name, _buf); \
        } \
    } while (0)

/*
 * SEL4_ASSERT_NE(a, b, name) — pass if (int64_t)(a) != (int64_t)(b)
 */
#define SEL4_ASSERT_NE(a, b, name) \
    do { \
        int64_t _a = (int64_t)(a); \
        int64_t _b = (int64_t)(b); \
        if (_a != _b) { \
            _SEL4_OK(name); \
        } else { \
            char _buf[128]; \
            snprintf(_buf, sizeof(_buf), "expected != %lld but got equal", \
                     (long long)_b); \
            _SEL4_FAIL(name, _buf); \
        } \
    } while (0)

/*
 * SEL4_ASSERT_OK(rc, name) — pass if rc == 0 (VOS_ERR_OK)
 */
#define SEL4_ASSERT_OK(rc, name) \
    do { \
        uint32_t _rc = (uint32_t)(rc); \
        if (_rc == 0u) { \
            _SEL4_OK(name); \
        } else { \
            char _buf[64]; \
            snprintf(_buf, sizeof(_buf), "expected VOS_ERR_OK(0) got %u", _rc); \
            _SEL4_FAIL(name, _buf); \
        } \
    } while (0)

/*
 * SEL4_ASSERT_TRUE(expr, name) — pass if expr evaluates to true
 */
#define SEL4_ASSERT_TRUE(expr, name) \
    do { \
        if (!!(expr)) { \
            _SEL4_OK(name); \
        } else { \
            _SEL4_FAIL(name, "expression is false"); \
        } \
    } while (0)
