/*
 * log_drain_test.c — contract tests for the LogDrain PD
 *
 * Covered opcodes:
 *   OP_LOG_WRITE  (0x01) — register ring slot and flush buffered output
 *   OP_LOG_STATUS (0x02) — query: slot_count, bytes_drained
 *
 * Channel: CH_LOG_DRAIN (55 on qemu-virt-aarch64, 60 otherwise).
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_log_drain_tests(microkit_channel ch)
{
    TEST_SECTION("log_drain");

    /* STATUS — must return slot_count and bytes_drained. */
    ASSERT_IPC_OK(ch, OP_LOG_STATUS, "log_drain: STATUS returns ok");

    /*
     * WRITE — register slot 0 with pd_id=0.
     * Expect ok or INVAL (if slot 0 is already in use by a real PD).
     */
    microkit_mr_set(0, (uint64_t)OP_LOG_WRITE);
    microkit_mr_set(1, 0);  /* slot */
    microkit_mr_set(2, 0);  /* pd_id */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_LOG_WRITE, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_INVAL || rc == AOS_ERR_EXISTS) {
            _tf_ok("log_drain: WRITE returns ok, inval, or exists");
        } else {
            _tf_fail_point("log_drain: WRITE returns ok, inval, or exists",
                           "unexpected error code");
        }
    }

    /* STATUS again — confirm log_drain still responds after WRITE. */
    ASSERT_IPC_OK(ch, OP_LOG_STATUS, "log_drain: STATUS still ok after WRITE");
}
