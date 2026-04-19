/*
 * net_isolator_test.c — contract tests for the NetIsolator PD
 *
 * Covered opcodes:
 *   0x01 — NET_ISOLATE   : add an isolation rule for an agent
 *   0x02 — NET_PERMIT    : remove an isolation rule
 *   0x03 — NET_STATUS    : query isolation table state
 *
 * Channel: CH_CONTROLLER_NET_ISOLATOR (61) from the controller's perspective.
 *
 * TODO: replace opcode literals with
 *       #include "../../contracts/net-isolator/interface.h"
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

#define NET_OP_ISOLATE  0x01u
#define NET_OP_PERMIT   0x02u
#define NET_OP_STATUS   0x03u

/* Controller-side channel to net_isolator (from channels_generated.h: 61) */
#define CH_NET_ISOLATOR_CTRL 61u

void run_net_isolator_tests(microkit_channel ch)
{
    TEST_SECTION("net_isolator");

    microkit_channel eff_ch = (ch == 0) ? (microkit_channel)CH_NET_ISOLATOR_CTRL : ch;

    if (eff_ch == 0) {
        _tf_puts("# net_isolator: channel not resolved\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - net_isolator: STATUS channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - net_isolator: ISOLATE channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - net_isolator: PERMIT channel placeholder # TODO wire ch\n");
        return;
    }

    /* STATUS — isolation table state. */
    ASSERT_IPC_OK(eff_ch, NET_OP_STATUS, "net_isolator: STATUS returns ok");

    /* ISOLATE — add rule for agent_id=0. */
    microkit_mr_set(0, (uint64_t)NET_OP_ISOLATE);
    microkit_mr_set(1, 0);  /* agent_id */
    (void)microkit_ppcall(eff_ch, microkit_msginfo_new(NET_OP_ISOLATE, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_EXISTS || rc == AOS_ERR_NOSPC) {
            _tf_ok("net_isolator: ISOLATE agent 0 returns ok, exists, or nospc");
        } else {
            _tf_fail_point("net_isolator: ISOLATE agent 0 returns ok, exists, or nospc",
                           "unexpected error code");
        }
    }

    /* PERMIT — remove rule for agent_id=0. */
    microkit_mr_set(0, (uint64_t)NET_OP_PERMIT);
    microkit_mr_set(1, 0);
    (void)microkit_ppcall(eff_ch, microkit_msginfo_new(NET_OP_PERMIT, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("net_isolator: PERMIT agent 0 returns ok or not-found");
        } else {
            _tf_fail_point("net_isolator: PERMIT agent 0 returns ok or not-found",
                           "unexpected error code");
        }
    }
}
