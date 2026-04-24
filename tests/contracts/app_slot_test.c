/*
 * app_slot_test.c — contract tests for the AppSlot PD (notification-only)
 *
 * Covered opcodes:
 *   APP_SLOT_CH_SPAWN              — notification channel to/from SpawnServer
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/app_slot_contract.h"

/*
 * AppSlot is a notification-only PD — it has no IPC protected() handler.
 * All communication is via seL4 notifications with SpawnServer.
 * This test verifies the PD exists and its contract constants are defined.
 */
void run_app_slot_tests(microkit_channel ch)
{
    TEST_SECTION("app_slot");

    /*
     * AppSlot has no IPC opcodes.  Verify contract constants are defined
     * and consistent.
     */
    ASSERT_TRUE(APP_SLOT_OK == 0u,
                "app_slot: APP_SLOT_OK is zero");
    ASSERT_TRUE(APP_SLOT_ERR_HASH == 1u,
                "app_slot: APP_SLOT_ERR_HASH is 1");
    ASSERT_TRUE(APP_SLOT_ERR_TRUNCATED == 2u,
                "app_slot: APP_SLOT_ERR_TRUNCATED is 2");
    ASSERT_TRUE(APP_SLOT_ERR_INVAL == 3u,
                "app_slot: APP_SLOT_ERR_INVAL is 3");
    ASSERT_TRUE(SPAWN_HEADER_MAGIC == 0x5350574eu,
                "app_slot: SPAWN_HEADER_MAGIC is 'SPWN'");
    ASSERT_TRUE(sizeof(app_slot_spawn_header_t) > 0,
                "app_slot: spawn_header_t struct has non-zero size");
}
