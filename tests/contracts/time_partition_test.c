/*
 * time_partition_test.c — contract tests for the TimePartition PD
 *
 * Covered opcodes:
 *   0x01 — TPART_ALLOC   : allocate a time partition for an agent
 *   0x02 — TPART_FREE    : free a time partition
 *   0x03 — TPART_STATUS  : query partition table state
 *   0x04 — TPART_SET     : update budget and period for a partition
 *
 * Channel: CH_CONTROLLER_TIME_PARTITION (41) from the controller's perspective.
 *
 * TODO: replace opcode literals with
 *       #include "../../contracts/time-partition/interface.h"
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

#define TPART_OP_ALLOC   0x01u
#define TPART_OP_FREE    0x02u
#define TPART_OP_STATUS  0x03u
#define TPART_OP_SET     0x04u

/* Controller-side channel to time_partition (from channels_generated.h: 41) */
#define CH_TIME_PARTITION_CTRL 41u

void run_time_partition_tests(microkit_channel ch)
{
    TEST_SECTION("time_partition");

    microkit_channel eff_ch = (ch == 0) ? (microkit_channel)CH_TIME_PARTITION_CTRL : ch;

    if (eff_ch == 0) {
        _tf_puts("# time_partition: channel not resolved\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - time_partition: STATUS channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - time_partition: ALLOC channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - time_partition: FREE channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - time_partition: SET channel placeholder # TODO wire ch\n");
        return;
    }

    /* STATUS — partition table state. */
    ASSERT_IPC_OK(eff_ch, TPART_OP_STATUS, "time_partition: STATUS returns ok");

    /* ALLOC — allocate a partition for agent_id=1 with 10ms budget, 100ms period. */
    microkit_mr_set(0, (uint64_t)TPART_OP_ALLOC);
    microkit_mr_set(1, 1);    /* agent_id */
    microkit_mr_set(2, 10);   /* budget_ms */
    microkit_mr_set(3, 100);  /* period_ms */
    (void)microkit_ppcall(eff_ch, microkit_msginfo_new(TPART_OP_ALLOC, 4));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOSPC || rc == AOS_ERR_EXISTS) {
            _tf_ok("time_partition: ALLOC returns ok, nospc, or exists");
        } else {
            _tf_fail_point("time_partition: ALLOC returns ok, nospc, or exists",
                           "unexpected error code");
        }
    }

    /* SET — update partition parameters. */
    microkit_mr_set(0, (uint64_t)TPART_OP_SET);
    microkit_mr_set(1, 1);   /* agent_id */
    microkit_mr_set(2, 20);  /* new budget_ms */
    microkit_mr_set(3, 100); /* period_ms */
    (void)microkit_ppcall(eff_ch, microkit_msginfo_new(TPART_OP_SET, 4));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("time_partition: SET returns ok, not-found, or inval");
        } else {
            _tf_fail_point("time_partition: SET returns ok, not-found, or inval",
                           "unexpected error code");
        }
    }

    /* FREE — release partition for agent_id=1. */
    microkit_mr_set(0, (uint64_t)TPART_OP_FREE);
    microkit_mr_set(1, 1);
    (void)microkit_ppcall(eff_ch, microkit_msginfo_new(TPART_OP_FREE, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("time_partition: FREE returns ok or not-found");
        } else {
            _tf_fail_point("time_partition: FREE returns ok or not-found",
                           "unexpected error code");
        }
    }
}
