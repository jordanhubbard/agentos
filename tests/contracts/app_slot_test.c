/*
 * app_slot_test.c — contract tests for the AppSlot PD (notification-only)
 *
 * AppSlot is a passive, notification-driven PD with no IPC protected()
 * handler.  This test verifies that the contract constants defined in
 * app_slot_contract.h are present and have the expected values.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/app_slot_contract.h"

void run_app_slot_tests(microkit_channel ch)
{
    (void)ch;  /* notification-only PD — no IPC channel exercised */
    TEST_SECTION("app_slot");

    /* APP_SLOT_OK must be zero (success). */
    if (APP_SLOT_OK == 0u) {
        _tf_ok("app_slot: APP_SLOT_OK == 0");
    } else {
        _tf_fail_point("app_slot: APP_SLOT_OK == 0", "value is not zero");
    }

    /* APP_SLOT_ERR_HASH must be 1. */
    if (APP_SLOT_ERR_HASH == 1u) {
        _tf_ok("app_slot: APP_SLOT_ERR_HASH == 1");
    } else {
        _tf_fail_point("app_slot: APP_SLOT_ERR_HASH == 1", "unexpected value");
    }

    /* APP_SLOT_ERR_TRUNCATED must be 2. */
    if (APP_SLOT_ERR_TRUNCATED == 2u) {
        _tf_ok("app_slot: APP_SLOT_ERR_TRUNCATED == 2");
    } else {
        _tf_fail_point("app_slot: APP_SLOT_ERR_TRUNCATED == 2", "unexpected value");
    }

    /* APP_SLOT_ERR_INVAL must be 3. */
    if (APP_SLOT_ERR_INVAL == 3u) {
        _tf_ok("app_slot: APP_SLOT_ERR_INVAL == 3");
    } else {
        _tf_fail_point("app_slot: APP_SLOT_ERR_INVAL == 3", "unexpected value");
    }

    /* SPAWN_HEADER_MAGIC must be 0x5350574e ("SPWN"). */
    if (SPAWN_HEADER_MAGIC == 0x5350574eu) {
        _tf_ok("app_slot: SPAWN_HEADER_MAGIC == 0x5350574e (\"SPWN\")");
    } else {
        _tf_fail_point("app_slot: SPAWN_HEADER_MAGIC == 0x5350574e (\"SPWN\")",
                       "unexpected magic value");
    }

    /* app_slot_spawn_header_t must be a non-zero-sized struct. */
    if (sizeof(app_slot_spawn_header_t) > 0u) {
        _tf_ok("app_slot: spawn_header_t has non-zero size");
    } else {
        _tf_fail_point("app_slot: spawn_header_t has non-zero size",
                       "struct has zero size");
    }
}
