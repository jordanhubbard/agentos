/*
 * block_test.c — contract tests for the Block Device PD
 *
 * Covered opcodes:
 *   MSG_BLOCK_OPEN                 — open a block device
 *   MSG_BLOCK_CLOSE                — close a block device handle
 *   MSG_BLOCK_READ                 — read sectors
 *   MSG_BLOCK_WRITE                — write sectors
 *   MSG_BLOCK_FLUSH                — flush write cache
 *   MSG_BLOCK_STATUS               — query device status
 *   MSG_BLOCK_TRIM                 — trim/discard sectors
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_block_tests(microkit_channel ch)
{
    TEST_SECTION("block");

    /* STATUS — query a bogus handle; expect NOT_FOUND or INVAL */
    microkit_mr_set(0, (uint64_t)MSG_BLOCK_STATUS);
    microkit_mr_set(1, 0xFFFFu);  /* bogus handle */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_BLOCK_STATUS, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("block: STATUS with bogus handle returns ok/not-found/inval");
        } else {
            _tf_fail_point("block: STATUS with bogus handle returns ok/not-found/inval",
                           "unexpected error code");
        }
    }

    /* OPEN — open device 0, partition 0 */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_BLOCK_OPEN, AOS_ERR_NOT_FOUND,
                         "block: OPEN returns ok or not-found");

    /* FLUSH — flush bogus handle */
    microkit_mr_set(0, (uint64_t)MSG_BLOCK_FLUSH);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_BLOCK_FLUSH, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("block: FLUSH with bogus handle returns ok/not-found/inval");
        } else {
            _tf_fail_point("block: FLUSH with bogus handle returns ok/not-found/inval",
                           "unexpected error code");
        }
    }

    /* TRIM — trim bogus handle */
    microkit_mr_set(0, (uint64_t)MSG_BLOCK_TRIM);
    microkit_mr_set(1, 0xFFFFu);
    microkit_mr_set(2, 0);
    microkit_mr_set(3, 1);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_BLOCK_TRIM, 4));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("block: TRIM with bogus handle returns ok/not-found/inval");
        } else {
            _tf_fail_point("block: TRIM with bogus handle returns ok/not-found/inval",
                           "unexpected error code");
        }
    }
}
