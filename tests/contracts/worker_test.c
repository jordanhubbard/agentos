/*
 * worker_test.c — contract tests for Worker pool PDs
 *
 * Covered opcodes:
 *   MSG_WORKER_RETRIEVE      (0x0701) — ask the controller to GET from AgentFS
 *   MSG_WORKER_RETRIEVE_REPLY(0x0702) — expected reply tag
 *   MSG_DEMO_TASK_RETRIEVE   (0x0710) — demo task: retrieve object from AgentFS
 *
 * Channel: WORKER_POOL_BASE_CH (20) — first worker PD from the controller's
 * perspective.  Tests target worker_0 only; the pool is symmetric so one
 * worker exercises the contract for all.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_worker_tests(microkit_channel ch)
{
    TEST_SECTION("worker");

    /*
     * MSG_WORKER_RETRIEVE — submit a retrieve task with a zero hash.
     * The worker should return ok (task queued) or not-found (AgentFS miss).
     */
    microkit_mr_set(0, (uint64_t)MSG_WORKER_RETRIEVE);
    microkit_mr_set(1, 0);  /* hash_lo */
    microkit_mr_set(2, 0);  /* hash_hi */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_WORKER_RETRIEVE, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_BUSY) {
            _tf_ok("worker: RETRIEVE returns ok, not-found, or busy");
        } else {
            _tf_fail_point("worker: RETRIEVE returns ok, not-found, or busy",
                           "unexpected error code");
        }
    }

    /*
     * MSG_DEMO_TASK_RETRIEVE — demo task opcode; same semantics as RETRIEVE
     * but tagged differently for pipeline demonstration.
     */
    microkit_mr_set(0, (uint64_t)MSG_DEMO_TASK_RETRIEVE);
    microkit_mr_set(1, 0);
    microkit_mr_set(2, 0);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_DEMO_TASK_RETRIEVE, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND ||
            rc == AOS_ERR_BUSY  || rc == AOS_ERR_UNIMPL) {
            _tf_ok("worker: DEMO_TASK_RETRIEVE returns structured response");
        } else {
            _tf_fail_point("worker: DEMO_TASK_RETRIEVE returns structured response",
                           "unexpected error code");
        }
    }

    /*
     * MSG_WORKER_RETRIEVE_REPLY — this tag flows worker→controller; testing
     * it on the controller→worker channel should return AOS_ERR_UNIMPL.
     */
    ASSERT_IPC_ERR(ch, MSG_WORKER_RETRIEVE_REPLY, AOS_ERR_UNIMPL,
                   "worker: RETRIEVE_REPLY on wrong direction returns unimpl");
}
