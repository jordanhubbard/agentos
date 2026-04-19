/*
 * test_framework.h — agentOS Phase 5 on-target TAP test framework
 *
 * This framework targets real Microkit protection domains running on seL4 or
 * the in-process simulator.  It does NOT use libc stdio; all output goes
 * through microkit_dbg_puts so that it works in no-libc PD environments.
 *
 * TAP output format (TAP version 14):
 *   ok N - <name>
 *   not ok N - <name>
 *   # <diagnostic>
 *   1..N              (emitted by tf_tap_finish)
 *
 * Typical use:
 *
 *   void init(void) {
 *       tf_tap_init("my-suite");
 *       ASSERT_IPC_OK(CH_EVENTBUS_CTRL, MSG_EVENTBUS_STATUS, "eventbus status ok");
 *       tf_tap_finish();
 *   }
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <microkit.h>

/* ── Internal state ──────────────────────────────────────────────────────── */

static int    _tf_pass  = 0;
static int    _tf_fail  = 0;
static int    _tf_total = 0;

/* ── Minimal numeric-to-string helpers (no libc) ─────────────────────────── */

static inline void _tf_puts(const char *s) {
    microkit_dbg_puts(s);
}

static inline void _tf_put_uint(uint64_t v) {
    if (v == 0) { microkit_dbg_puts("0"); return; }
    char buf[21];
    int  i = 20;
    buf[i] = '\0';
    while (v > 0 && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    microkit_dbg_puts(&buf[i]);
}

static inline void _tf_put_hex(uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    microkit_dbg_puts("0x");
    char buf[17];
    int  i = 16;
    buf[i] = '\0';
    if (v == 0) { microkit_dbg_puts("0"); return; }
    while (v > 0 && i > 0) {
        buf[--i] = hex[v & 0xF];
        v >>= 4;
    }
    microkit_dbg_puts(&buf[i]);
}

/* ── TAP lifecycle ───────────────────────────────────────────────────────── */

/*
 * tf_tap_init — emit a TAP header comment naming the suite.
 * Call once before any ASSERT_* macros.
 */
static inline void tf_tap_init(const char *suite_name) {
    _tf_puts("TAP version 14\n# suite: ");
    _tf_puts(suite_name);
    _tf_puts("\n");
}

/*
 * tf_tap_finish — emit plan line and summary; call once at end of suite.
 *
 * Emits:
 *   1..N
 *   # passed: P
 *   # failed: F
 */
static inline void tf_tap_finish(void) {
    _tf_puts("1..");
    _tf_put_uint((uint64_t)_tf_total);
    _tf_puts("\n# passed: ");
    _tf_put_uint((uint64_t)_tf_pass);
    _tf_puts("\n# failed: ");
    _tf_put_uint((uint64_t)_tf_fail);
    _tf_puts("\n");
}

/* ── Internal test-point emitters ────────────────────────────────────────── */

static inline void _tf_ok(const char *name) {
    _tf_total++;
    _tf_pass++;
    _tf_puts("ok ");
    _tf_put_uint((uint64_t)_tf_total);
    _tf_puts(" - ");
    _tf_puts(name);
    _tf_puts("\n");
}

static inline void _tf_fail_point(const char *name, const char *diag) {
    _tf_total++;
    _tf_fail++;
    _tf_puts("not ok ");
    _tf_put_uint((uint64_t)_tf_total);
    _tf_puts(" - ");
    _tf_puts(name);
    _tf_puts("\n  # ");
    _tf_puts(diag);
    _tf_puts("\n");
}

/* ── Section comment ─────────────────────────────────────────────────────── */

/*
 * TEST_SECTION(name) — emit a TAP diagnostic naming the current section.
 * Does not count as a test point.
 */
#define TEST_SECTION(name) \
    do { _tf_puts("# --- " name " ---\n"); } while (0)

/* ── Core assertion macros ───────────────────────────────────────────────── */

/*
 * ASSERT_IPC_OK(ch, op, name)
 *
 * Set MR0 = op, issue a PPC on channel ch, assert that reply MR0 == 0 (ok).
 * On failure the diagnostic shows the actual MR0 value.
 */
#define ASSERT_IPC_OK(ch, op, name)                                     \
    do {                                                                 \
        microkit_mr_set(0, (uint64_t)(op));                             \
        microkit_msginfo _reply =                                        \
            microkit_ppcall((ch), microkit_msginfo_new((uint64_t)(op), 1)); \
        (void)_reply;                                                    \
        uint64_t _status = microkit_mr_get(0);                          \
        if (_status == 0) {                                              \
            _tf_ok(name);                                                \
        } else {                                                         \
            _tf_puts("  # ASSERT_IPC_OK: MR0=");                        \
            _tf_put_hex(_status);                                        \
            _tf_puts(" (expected 0x0)\n");                               \
            _tf_fail_point(name, "reply MR0 != 0");                     \
        }                                                                \
    } while (0)

/*
 * ASSERT_IPC_ERR(ch, op, err, name)
 *
 * Set MR0 = op, issue a PPC on channel ch, assert that reply MR0 == err.
 * Useful for testing expected-error paths (e.g. not-implemented).
 */
#define ASSERT_IPC_ERR(ch, op, err, name)                               \
    do {                                                                 \
        microkit_mr_set(0, (uint64_t)(op));                             \
        microkit_msginfo _reply =                                        \
            microkit_ppcall((ch), microkit_msginfo_new((uint64_t)(op), 1)); \
        (void)_reply;                                                    \
        uint64_t _status = microkit_mr_get(0);                          \
        uint64_t _want   = (uint64_t)(err);                             \
        if (_status == _want) {                                          \
            _tf_ok(name);                                                \
        } else {                                                         \
            _tf_puts("  # ASSERT_IPC_ERR: MR0=");                       \
            _tf_put_hex(_status);                                        \
            _tf_puts(" (expected ");                                      \
            _tf_put_hex(_want);                                          \
            _tf_puts(")\n");                                             \
            _tf_fail_point(name, "reply MR0 != expected error");        \
        }                                                                \
    } while (0)

/*
 * ASSERT_REPLY_U32(ch, op, mr, val, name)
 *
 * Set MR0 = op, issue a PPC on channel ch.  Assert that reply MR[mr] == val.
 * Also asserts MR0 == 0 (operation succeeded) before checking the field.
 */
#define ASSERT_REPLY_U32(ch, op, mr, val, name)                         \
    do {                                                                 \
        microkit_mr_set(0, (uint64_t)(op));                             \
        microkit_msginfo _reply =                                        \
            microkit_ppcall((ch), microkit_msginfo_new((uint64_t)(op), 1)); \
        (void)_reply;                                                    \
        uint64_t _status = microkit_mr_get(0);                          \
        uint64_t _got    = microkit_mr_get((uint32_t)(mr));             \
        uint64_t _want   = (uint64_t)(val);                             \
        if (_status != 0) {                                              \
            _tf_puts("  # ASSERT_REPLY_U32: op returned MR0=");         \
            _tf_put_hex(_status);                                        \
            _tf_puts("\n");                                              \
            _tf_fail_point(name, "operation failed (MR0 != 0)");        \
        } else if (_got == _want) {                                      \
            _tf_ok(name);                                                \
        } else {                                                         \
            _tf_puts("  # ASSERT_REPLY_U32: MR");                       \
            _tf_put_uint((uint64_t)(mr));                                \
            _tf_puts("=");                                               \
            _tf_put_hex(_got);                                           \
            _tf_puts(" (expected ");                                     \
            _tf_put_hex(_want);                                          \
            _tf_puts(")\n");                                             \
            _tf_fail_point(name, "reply register value mismatch");      \
        }                                                                \
    } while (0)

/*
 * ASSERT_REPLY_NONZERO(ch, op, mr, name)
 *
 * Set MR0 = op, issue a PPC, assert MR0==0 and MR[mr] != 0.
 * Useful for status queries that return counts or handles.
 */
#define ASSERT_REPLY_NONZERO(ch, op, mr, name)                          \
    do {                                                                 \
        microkit_mr_set(0, (uint64_t)(op));                             \
        microkit_msginfo _reply =                                        \
            microkit_ppcall((ch), microkit_msginfo_new((uint64_t)(op), 1)); \
        (void)_reply;                                                    \
        uint64_t _status = microkit_mr_get(0);                          \
        uint64_t _got    = microkit_mr_get((uint32_t)(mr));             \
        if (_status != 0) {                                              \
            _tf_fail_point(name, "operation failed (MR0 != 0)");        \
        } else if (_got != 0) {                                         \
            _tf_ok(name);                                                \
        } else {                                                         \
            _tf_fail_point(name, "reply register is zero");             \
        }                                                                \
    } while (0)

/*
 * ASSERT_IPC_OK_OR_ERR(ch, op, ok_err, name)
 *
 * Accept MR0 == 0 OR MR0 == ok_err as a passing result.
 * Used for operations that may not be implemented yet (AOS_ERR_UNIMPL).
 */
#define ASSERT_IPC_OK_OR_ERR(ch, op, ok_err, name)                     \
    do {                                                                 \
        microkit_mr_set(0, (uint64_t)(op));                             \
        microkit_msginfo _reply =                                        \
            microkit_ppcall((ch), microkit_msginfo_new((uint64_t)(op), 1)); \
        (void)_reply;                                                    \
        uint64_t _status = microkit_mr_get(0);                          \
        if (_status == 0 || _status == (uint64_t)(ok_err)) {           \
            _tf_ok(name);                                                \
        } else {                                                         \
            _tf_puts("  # ASSERT_IPC_OK_OR_ERR: MR0=");                 \
            _tf_put_hex(_status);                                        \
            _tf_puts("\n");                                              \
            _tf_fail_point(name, "unexpected error code in MR0");       \
        }                                                                \
    } while (0)

/* ── Common agentOS error codes ──────────────────────────────────────────── */

#define AOS_OK            0x00u
#define AOS_ERR_NOT_FOUND 0x01u
#define AOS_ERR_EXISTS    0x02u
#define AOS_ERR_PERM      0x03u
#define AOS_ERR_INVAL     0x04u
#define AOS_ERR_NOSPC     0x05u
#define AOS_ERR_TIMEOUT   0x06u
#define AOS_ERR_BUSY      0x07u
#define AOS_ERR_TOO_LARGE 0x08u
#define AOS_ERR_UNIMPL    0x09u
