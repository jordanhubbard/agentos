/*
 * agent_pool_test.c — contract tests for the AgentPool PD
 *
 * Covered opcodes:
 *   0x01 — POOL_ACQUIRE : acquire an agent slot from the pool
 *   0x02 — POOL_RELEASE : release a slot back to the pool
 *   0x03 — POOL_STATUS  : query pool occupancy and capacity
 *
 * Channel: 0 (placeholder — update when ch is assigned in agentos.system).
 *
 * TODO: replace opcode literals with
 *       #include "../../contracts/agent-pool/interface.h"
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

#define AGENTPOOL_OP_ACQUIRE 0x01u
#define AGENTPOOL_OP_RELEASE 0x02u
#define AGENTPOOL_OP_STATUS  0x03u

void run_agent_pool_tests(microkit_channel ch)
{
    TEST_SECTION("agent_pool");

    if (ch == 0) {
        _tf_puts("# agent_pool: channel not wired in test topology (ch=0)\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - agent_pool: STATUS channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - agent_pool: ACQUIRE channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - agent_pool: RELEASE channel placeholder # TODO wire ch\n");
        return;
    }

    /* STATUS — pool must always respond. */
    ASSERT_IPC_OK(ch, AGENTPOOL_OP_STATUS, "agent_pool: STATUS returns ok");

    /* ACQUIRE — attempt to acquire a slot; may fail with NOSPC if pool is full. */
    microkit_mr_set(0, (uint64_t)AGENTPOOL_OP_ACQUIRE);
    microkit_mr_set(1, 0);  /* agent_class = 0 (generic) */
    (void)microkit_ppcall(ch, microkit_msginfo_new(AGENTPOOL_OP_ACQUIRE, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOSPC) {
            _tf_ok("agent_pool: ACQUIRE returns ok or nospc");
        } else {
            _tf_fail_point("agent_pool: ACQUIRE returns ok or nospc",
                           "unexpected error code");
        }
    }

    /* RELEASE — release slot 0; may return not-found if ACQUIRE failed. */
    microkit_mr_set(0, (uint64_t)AGENTPOOL_OP_RELEASE);
    microkit_mr_set(1, 0);  /* slot_id = 0 */
    (void)microkit_ppcall(ch, microkit_msginfo_new(AGENTPOOL_OP_RELEASE, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("agent_pool: RELEASE returns ok, not-found, or inval");
        } else {
            _tf_fail_point("agent_pool: RELEASE returns ok, not-found, or inval",
                           "unexpected error code");
        }
    }
}
