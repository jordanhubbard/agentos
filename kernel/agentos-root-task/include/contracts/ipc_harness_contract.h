/*
 * IPCHarness IPC Contract
 *
 * The IPCHarness PD is a programmatic test driver compiled into debug/test
 * builds (CONFIG_IPC_HARNESS=1).  On boot, it executes a static sequence
 * of IPC calls against configured target PDs and reports results via LogDrain.
 *
 * Channel: CH_IPC_HARNESS (see agentos.h) — test builds only
 * Opcodes: MSG_TEST_RUN, MSG_TEST_STATUS, MSG_TEST_RESULT
 *
 * Invariants:
 *   - IPCHarness is EXCLUDED from production images (CONFIG_IPC_HARNESS=0).
 *   - It accepts NO terminal input; all I/O is via MSG_LOG_WRITE to LogDrain.
 *   - MSG_TEST_RUN triggers a test suite by suite_id; 0 = all suites.
 *   - MSG_TEST_STATUS is read-only; it returns pass/fail/skip counts.
 *   - MSG_TEST_RESULT queries the result of a specific test by test_id.
 *   - The harness exits cleanly after completing its test sequence.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define IPC_HARNESS_CH_CONTROLLER  CH_IPC_HARNESS

/* ─── Request structs ────────────────────────────────────────────────────── */

struct test_req_run {
    uint32_t suite_id;          /* 0 = all suites */
    uint32_t flags;             /* TEST_RUN_FLAG_* */
};

#define TEST_RUN_FLAG_VERBOSE   (1u << 0)  /* emit per-assertion output to log drain */
#define TEST_RUN_FLAG_STOP_FAIL (1u << 1)  /* stop on first failure */

struct test_req_status {
    /* no fields */
};

struct test_req_result {
    uint32_t test_id;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct test_reply_run {
    uint32_t ok;
    uint32_t tests_run;
};

struct test_reply_status {
    uint32_t ok;
    uint32_t pass;
    uint32_t fail;
    uint32_t skip;
    uint32_t suites_run;
};

struct test_reply_result {
    uint32_t ok;
    uint32_t result;            /* TEST_RESULT_* */
    /* test name NUL-terminated string in shmem */
};

#define TEST_RESULT_PASS   0
#define TEST_RESULT_FAIL   1
#define TEST_RESULT_SKIP   2
#define TEST_RESULT_NOTRUN 3

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum ipc_harness_error {
    TEST_OK                   = 0,
    TEST_ERR_BAD_SUITE        = 1,
    TEST_ERR_BAD_TEST_ID      = 2,
    TEST_ERR_NOT_RUN          = 3,  /* status queried before any suite run */
};
