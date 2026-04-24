/*
 * app_manager_test.c — contract tests for the AppManager PD
 *
 * Covered opcodes:
 *   OP_APP_LAUNCH                  — launch an application
 *   OP_APP_KILL                    — kill a running application
 *   OP_APP_STATUS                  — query application status
 *   OP_APP_LIST                    — list running applications
 *   OP_APP_HEALTH                  — health check
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_app_manager_tests(microkit_channel ch)
{
    TEST_SECTION("app_manager");

    /* HEALTH — basic liveness check */
    ASSERT_IPC_OK(ch, OP_APP_HEALTH, "app_manager: HEALTH returns ok");

    /* LIST — enumerate running apps (may be empty) */
    ASSERT_IPC_OK(ch, OP_APP_LIST, "app_manager: LIST returns ok");

    /* STATUS — query a non-existent app; expect NOT_FOUND or INVAL */
    microkit_mr_set(0, (uint64_t)OP_APP_STATUS);
    microkit_mr_set(1, 0xFFFFu);  /* bogus app_id */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_APP_STATUS, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("app_manager: STATUS with bad id returns not-found or inval");
        } else {
            _tf_fail_point("app_manager: STATUS with bad id returns not-found or inval",
                           "unexpected error code");
        }
    }

    /* KILL — kill a non-existent app; expect NOT_FOUND or INVAL */
    microkit_mr_set(0, (uint64_t)OP_APP_KILL);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_APP_KILL, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("app_manager: KILL with bad id returns not-found or inval");
        } else {
            _tf_fail_point("app_manager: KILL with bad id returns not-found or inval",
                           "unexpected error code");
        }
    }

    /* LAUNCH — requires a valid manifest in shmem; expect ok or UNIMPL */
    ASSERT_IPC_OK_OR_ERR(ch, OP_APP_LAUNCH, AOS_ERR_UNIMPL,
                         "app_manager: LAUNCH returns ok or unimpl");
}
