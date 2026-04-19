/*
 * init_agent_test.c — contract tests for the InitAgent PD
 *
 * Covered opcodes:
 *   MSG_INITAGENT_START    (0x0201) — start the init agent
 *   MSG_INITAGENT_SHUTDOWN (0x0202) — request graceful shutdown
 *   MSG_INITAGENT_STATUS   (0x0302) — query init agent state
 *
 * Channel: MONITOR_CH_INITAGENT (2) from the controller's perspective.
 *
 * Note: MSG_INITAGENT_STATUS uses the reply tag (0x0302) in the request
 * direction as the STATUS query opcode; this matches the convention used
 * throughout the agentOS IPC layer.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_init_agent_tests(microkit_channel ch)
{
    TEST_SECTION("init_agent");

    /* STATUS — query current state; must always respond. */
    ASSERT_IPC_OK(ch, MSG_INITAGENT_STATUS, "init_agent: STATUS returns ok");

    /* START — idempotent if already started; must not crash. */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_INITAGENT_START, AOS_ERR_BUSY,
                         "init_agent: START returns ok or busy");

    /* STATUS after START — init agent must still respond. */
    ASSERT_IPC_OK(ch, MSG_INITAGENT_STATUS,
                  "init_agent: STATUS still ok after START");

    /*
     * SHUTDOWN — we send the shutdown request but accept AOS_ERR_PERM
     * (controller may not have rights to stop init) or AOS_OK.
     * We do NOT proceed with actual shutdown to keep the test system alive.
     */
    microkit_mr_set(0, (uint64_t)MSG_INITAGENT_SHUTDOWN);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_INITAGENT_SHUTDOWN, 1));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_PERM || rc == AOS_ERR_BUSY) {
            _tf_ok("init_agent: SHUTDOWN accepted (ok/perm/busy)");
        } else {
            _tf_fail_point("init_agent: SHUTDOWN accepted (ok/perm/busy)",
                           "unexpected error code");
        }
    }
}
