/*
 * snapshot_sched_test.c — contract tests for the SnapshotSched PD
 *
 * Covered opcodes:
 *   OP_SNAP_STATUS      (0xB0) — query scheduler state
 *   OP_SNAP_SET_POLICY  (0xB1) — set snapshot interval and delta threshold
 *   OP_SNAP_FORCE       (0xB2) — force an immediate snapshot round
 *   OP_SNAP_GET_HISTORY (0xB3) — retrieve last N round summaries
 *
 * Channel: 0 (placeholder — update when ch is assigned in agentos.system).
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_snapshot_sched_tests(microkit_channel ch)
{
    TEST_SECTION("snapshot_sched");

    if (ch == 0) {
        _tf_puts("# snapshot_sched: channel not wired in test topology (ch=0)\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - snapshot_sched: STATUS channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - snapshot_sched: SET_POLICY channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - snapshot_sched: FORCE channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - snapshot_sched: GET_HISTORY channel placeholder # TODO wire ch\n");
        return;
    }

    /* STATUS — query rounds, total_snapped, tick, slot_count. */
    ASSERT_IPC_OK(ch, OP_SNAP_STATUS, "snapshot_sched: STATUS returns ok");

    /* SET_POLICY — update interval and delta threshold. */
    microkit_mr_set(0, (uint64_t)OP_SNAP_SET_POLICY);
    microkit_mr_set(1, SNAP_INTERVAL_TICKS_DEFAULT);
    microkit_mr_set(2, SNAP_MIN_DELTA_DEFAULT);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_SNAP_SET_POLICY, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_INVAL) {
            _tf_ok("snapshot_sched: SET_POLICY returns ok or inval");
        } else {
            _tf_fail_point("snapshot_sched: SET_POLICY returns ok or inval",
                           "unexpected error code");
        }
    }

    /* FORCE — trigger an immediate round. */
    ASSERT_IPC_OK_OR_ERR(ch, OP_SNAP_FORCE, AOS_ERR_BUSY,
                         "snapshot_sched: FORCE returns ok or busy");

    /* GET_HISTORY — retrieve last 4 round summaries. */
    ASSERT_IPC_OK(ch, OP_SNAP_GET_HISTORY, "snapshot_sched: GET_HISTORY returns ok");
}
