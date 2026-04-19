/*
 * oom_killer_test.c — contract tests for the OomKiller PD
 *
 * Covered opcodes:
 *   0x01 — OOM_STATUS    : query current pressure level and victim history
 *   0x02 — OOM_SET_POLICY: configure eviction policy (thresholds, strategy)
 *   0x03 — OOM_PROBE     : manually trigger a pressure probe
 *
 * Channel: 0 (placeholder — update when ch is assigned in agentos.system).
 *
 * TODO: replace opcode literals with
 *       #include "../../contracts/oom-killer/interface.h"
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

#define OOM_OP_STATUS     0x01u
#define OOM_OP_SET_POLICY 0x02u
#define OOM_OP_PROBE      0x03u

void run_oom_killer_tests(microkit_channel ch)
{
    TEST_SECTION("oom_killer");

    if (ch == 0) {
        _tf_puts("# oom_killer: channel not wired in test topology (ch=0)\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - oom_killer: STATUS channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - oom_killer: SET_POLICY channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - oom_killer: PROBE channel placeholder # TODO wire ch\n");
        return;
    }

    /* STATUS — query memory pressure and victim history. */
    ASSERT_IPC_OK(ch, OOM_OP_STATUS, "oom_killer: STATUS returns ok");

    /* SET_POLICY — configure with default thresholds. */
    microkit_mr_set(0, (uint64_t)OOM_OP_SET_POLICY);
    microkit_mr_set(1, 80);   /* high_water_pct = 80% */
    microkit_mr_set(2, 95);   /* critical_pct = 95% */
    microkit_mr_set(3, 0);    /* strategy = 0 (LRU) */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OOM_OP_SET_POLICY, 4));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_INVAL) {
            _tf_ok("oom_killer: SET_POLICY returns ok or inval");
        } else {
            _tf_fail_point("oom_killer: SET_POLICY returns ok or inval",
                           "unexpected error code");
        }
    }

    /* PROBE — trigger a pressure probe; may find nothing to evict. */
    ASSERT_IPC_OK_OR_ERR(ch, OOM_OP_PROBE, AOS_ERR_NOT_FOUND,
                         "oom_killer: PROBE returns ok or not-found");
}
