/*
 * exec_server_test.c — contract tests for the ExecServer PD
 *
 * Covered opcodes:
 *   OP_EXEC_LAUNCH                 — launch a process
 *   OP_EXEC_STATUS                 — query process status
 *   OP_EXEC_WAIT                   — wait for process exit
 *   OP_EXEC_KILL                   — kill a process
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_exec_server_tests(microkit_channel ch)
{
    TEST_SECTION("exec_server");

    /* STATUS — query bogus pid */
    microkit_mr_set(0, (uint64_t)OP_EXEC_STATUS);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_EXEC_STATUS, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("exec_server: STATUS with bad pid returns ok/not-found/inval");
        } else {
            _tf_fail_point("exec_server: STATUS with bad pid returns ok/not-found/inval",
                           "unexpected error code");
        }
    }

    /* KILL — kill bogus pid */
    microkit_mr_set(0, (uint64_t)OP_EXEC_KILL);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_EXEC_KILL, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("exec_server: KILL with bad pid returns not-found/inval");
        } else {
            _tf_fail_point("exec_server: KILL with bad pid returns not-found/inval",
                           "unexpected error code");
        }
    }

    /* LAUNCH — no valid binary staged; expect INVAL or UNIMPL */
    ASSERT_IPC_OK_OR_ERR(ch, OP_EXEC_LAUNCH, AOS_ERR_INVAL,
                         "exec_server: LAUNCH without binary returns inval or ok");

    /* WAIT — wait on bogus pid; expect NOT_FOUND */
    microkit_mr_set(0, (uint64_t)OP_EXEC_WAIT);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_EXEC_WAIT, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("exec_server: WAIT with bad pid returns not-found/inval");
        } else {
            _tf_fail_point("exec_server: WAIT with bad pid returns not-found/inval",
                           "unexpected error code");
        }
    }
}
