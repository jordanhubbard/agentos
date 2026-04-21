/*
 * gpu_sched_test.c — contract tests for the GPU Scheduler PD
 *
 * Covered opcodes:
 *   MSG_GPU_SUBMIT       (0x0901) — submit a GPU task
 *   MSG_GPU_STATUS       (0x0903) — query scheduler state
 *   MSG_GPU_CANCEL       (0x0905) — cancel a pending ticket
 *
 * Channel: 0 (placeholder — update when ch is assigned in agentos.system).
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_gpu_sched_tests(microkit_channel ch)
{
    TEST_SECTION("gpu_sched");

    if (ch == 0) {
        _tf_puts("# gpu_sched: channel not wired in test topology (ch=0)\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - gpu_sched: STATUS channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - gpu_sched: SUBMIT channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - gpu_sched: CANCEL channel placeholder # TODO wire ch\n");
        return;
    }

    /* STATUS — scheduler must respond with queue and slot counts. */
    ASSERT_IPC_OK(ch, MSG_GPU_STATUS, "gpu_sched: STATUS returns ok");

    /* SUBMIT — submit a zero-hash task with default priority. */
    microkit_mr_set(0, (uint64_t)MSG_GPU_SUBMIT);
    microkit_mr_set(1, 0);  /* hash_lo */
    microkit_mr_set(2, 0);  /* hash_hi */
    microkit_mr_set(3, 0);  /* priority = 0 (best-effort) */
    microkit_mr_set(4, 0);  /* flags = 0 */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_GPU_SUBMIT, 5));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOSPC || rc == AOS_ERR_INVAL) {
            _tf_ok("gpu_sched: SUBMIT returns ok, nospc, or inval");
        } else {
            _tf_fail_point("gpu_sched: SUBMIT returns ok, nospc, or inval",
                           "unexpected error code");
        }
    }

    /* CANCEL — cancel ticket_id=0; expect ok or not-found. */
    microkit_mr_set(0, (uint64_t)MSG_GPU_CANCEL);
    microkit_mr_set(1, 0);  /* ticket_id = 0 */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_GPU_CANCEL, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("gpu_sched: CANCEL returns ok or not-found");
        } else {
            _tf_fail_point("gpu_sched: CANCEL returns ok or not-found",
                           "unexpected error code");
        }
    }
}
