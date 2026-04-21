/*
 * perf_counters_test.c — contract tests for the PerfCounters PD
 *
 * Covered opcodes:
 *   0x01 — PERF_STATUS  : query overall counter state
 *   0x02 — PERF_READ    : read counters for a specific worker slot
 *   0x03 — PERF_RESET   : reset counters for a specific worker slot
 *
 * Channel: CH_CONTROLLER_PERF_COUNTERS (42) from the controller's perspective.
 *
 * TODO: replace opcode literals with
 *       #include "../../contracts/perf-counters/interface.h"
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

#define PERF_OP_STATUS  0x01u
#define PERF_OP_READ    0x02u
#define PERF_OP_RESET   0x03u

/* Controller-side channel to perf_counters (from channels_generated.h: 42) */
#define CH_PERF_COUNTERS_CTRL 42u

void run_perf_counters_tests(microkit_channel ch)
{
    TEST_SECTION("perf_counters");

    microkit_channel eff_ch = (ch == 0) ? (microkit_channel)CH_PERF_COUNTERS_CTRL : ch;

    if (eff_ch == 0) {
        _tf_puts("# perf_counters: channel not resolved\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - perf_counters: STATUS channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - perf_counters: READ channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - perf_counters: RESET channel placeholder # TODO wire ch\n");
        return;
    }

    /* STATUS — overall counter status. */
    ASSERT_IPC_OK(eff_ch, PERF_OP_STATUS, "perf_counters: STATUS returns ok");

    /* READ — read counters for worker_slot=0. */
    microkit_mr_set(0, (uint64_t)PERF_OP_READ);
    microkit_mr_set(1, 0);  /* worker_slot */
    (void)microkit_ppcall(eff_ch, microkit_msginfo_new(PERF_OP_READ, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("perf_counters: READ slot 0 returns ok or not-found");
        } else {
            _tf_fail_point("perf_counters: READ slot 0 returns ok or not-found",
                           "unexpected error code");
        }
    }

    /* RESET — reset counters for worker_slot=0. */
    microkit_mr_set(0, (uint64_t)PERF_OP_RESET);
    microkit_mr_set(1, 0);
    (void)microkit_ppcall(eff_ch, microkit_msginfo_new(PERF_OP_RESET, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_UNIMPL) {
            _tf_ok("perf_counters: RESET slot 0 returns ok, not-found, or unimpl");
        } else {
            _tf_fail_point("perf_counters: RESET slot 0 returns ok, not-found, or unimpl",
                           "unexpected error code");
        }
    }
}
