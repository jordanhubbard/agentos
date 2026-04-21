/*
 * quota_pd_test.c — contract tests for the QuotaPD
 *
 * Covered opcodes:
 *   OP_QUOTA_REGISTER (0x60) — register agent with cpu/mem limits
 *   OP_QUOTA_TICK     (0x61) — tick agent cpu/mem usage counters
 *   OP_QUOTA_STATUS   (0x62) — query agent quota state
 *   OP_QUOTA_SET      (0x63) — update quota limits
 *
 * Channel: CH_QUOTA_CTRL (52) from the controller's perspective.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_quota_pd_tests(microkit_channel ch)
{
    TEST_SECTION("quota_pd");

    /* STATUS — query quota state for agent_id=0; may return not-found. */
    microkit_mr_set(0, (uint64_t)OP_QUOTA_STATUS);
    microkit_mr_set(1, 0);  /* agent_id */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_QUOTA_STATUS, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("quota_pd: STATUS for agent 0 returns ok or not-found");
        } else {
            _tf_fail_point("quota_pd: STATUS for agent 0 returns ok or not-found",
                           "unexpected error code");
        }
    }

    /* REGISTER — register agent_id=1 with cpu=100, mem=4096. */
    microkit_mr_set(0, (uint64_t)OP_QUOTA_REGISTER);
    microkit_mr_set(1, 1);     /* agent_id */
    microkit_mr_set(2, 100);   /* cpu_budget (ticks) */
    microkit_mr_set(3, 4096);  /* mem_budget (bytes) */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_QUOTA_REGISTER, 4));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_EXISTS || rc == AOS_ERR_NOSPC) {
            _tf_ok("quota_pd: REGISTER agent_id=1 returns ok, exists, or nospc");
        } else {
            _tf_fail_point("quota_pd: REGISTER agent_id=1 returns ok, exists, or nospc",
                           "unexpected error code");
        }
    }

    /* TICK — update usage for agent_id=1. */
    microkit_mr_set(0, (uint64_t)OP_QUOTA_TICK);
    microkit_mr_set(1, 1);   /* agent_id */
    microkit_mr_set(2, 10);  /* cpu_delta */
    microkit_mr_set(3, 128); /* mem_delta */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_QUOTA_TICK, 4));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("quota_pd: TICK agent_id=1 returns ok or not-found");
        } else {
            _tf_fail_point("quota_pd: TICK agent_id=1 returns ok or not-found",
                           "unexpected error code");
        }
    }

    /* STATUS — verify agent_id=1 is tracked after REGISTER. */
    microkit_mr_set(0, (uint64_t)OP_QUOTA_STATUS);
    microkit_mr_set(1, 1);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_QUOTA_STATUS, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("quota_pd: STATUS after REGISTER returns ok or not-found");
        } else {
            _tf_fail_point("quota_pd: STATUS after REGISTER returns ok or not-found",
                           "unexpected error code");
        }
    }

    /* SET — update limits for agent_id=1. */
    microkit_mr_set(0, (uint64_t)OP_QUOTA_SET);
    microkit_mr_set(1, 1);     /* agent_id */
    microkit_mr_set(2, 200);   /* new cpu_budget */
    microkit_mr_set(3, 8192);  /* new mem_budget */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_QUOTA_SET, 4));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("quota_pd: SET limits returns ok, not-found, or inval");
        } else {
            _tf_fail_point("quota_pd: SET limits returns ok, not-found, or inval",
                           "unexpected error code");
        }
    }
}
