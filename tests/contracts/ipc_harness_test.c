/*
 * ipc_harness_test.c — contract tests for the IPC Test Harness PD
 *
 * Covered opcodes:
 *   MSG_TEST_RUN                   — run a test suite
 *   MSG_TEST_STATUS                — query test status
 *   MSG_TEST_RESULT                — get result for a specific test
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_ipc_harness_tests(microkit_channel ch)
{
    TEST_SECTION("ipc_harness");

    /* STATUS — query pass/fail/skip counts */
    ASSERT_IPC_OK(ch, MSG_TEST_STATUS, "ipc_harness: STATUS returns ok");

    /* RESULT — query bogus test_id */
    microkit_mr_set(0, (uint64_t)MSG_TEST_RESULT);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_TEST_RESULT, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("ipc_harness: RESULT with bogus id returns ok/not-found/inval");
        } else {
            _tf_fail_point("ipc_harness: RESULT with bogus id returns ok/not-found/inval",
                           "unexpected error code");
        }
    }

    /* RUN — run suite 0; expect ok or BUSY (if already running) */
    microkit_mr_set(0, (uint64_t)MSG_TEST_RUN);
    microkit_mr_set(1, 0);  /* suite_id */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_TEST_RUN, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_BUSY || rc == AOS_ERR_UNIMPL) {
            _tf_ok("ipc_harness: RUN returns ok/busy/unimpl");
        } else {
            _tf_fail_point("ipc_harness: RUN returns ok/busy/unimpl",
                           "unexpected error code");
        }
    }
}
