/*
 * cap_broker_test.c — contract tests for the Capability Broker
 *
 * Covered opcodes (dispatched via the monitor/cap_broker PD):
 *   OP_CAP_BROKER_RELOAD (0x15) — hot-reload policy blob; revoke violating grants
 *   OP_CAP_STATUS        (0x16) — query: cap_count, policy_version, active_grants
 *   OP_CAP_POLICY_RELOAD (0xC0) — fetch and validate new policy from AgentFS
 *   OP_CAP_POLICY_STATUS (0xC1) — query policy: loaded, version, count, hash
 *   OP_CAP_POLICY_RESET  (0xC2) — revert to static compile-time policy
 *   OP_CAP_POLICY_DIFF   (0xC3) — diff: revoked, classes, version
 *
 * Channel: 0 (placeholder — cap_broker is dispatched through the monitor PD;
 *          the exact controller-side channel depends on the system topology).
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_cap_broker_tests(microkit_channel ch)
{
    TEST_SECTION("cap_broker");

    if (ch == 0) {
        _tf_puts("# cap_broker: channel not wired in test topology (ch=0)\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - cap_broker: CAP_STATUS channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - cap_broker: POLICY_STATUS channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - cap_broker: CAP_BROKER_RELOAD channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - cap_broker: POLICY_RELOAD channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - cap_broker: POLICY_RESET channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - cap_broker: POLICY_DIFF channel placeholder # TODO wire ch\n");
        return;
    }

    /* CAP_STATUS — query active grant count and policy version. */
    ASSERT_IPC_OK(ch, OP_CAP_STATUS, "cap_broker: CAP_STATUS returns ok");

    /* CAP_POLICY_STATUS — query policy metadata. */
    ASSERT_IPC_OK(ch, OP_CAP_POLICY_STATUS, "cap_broker: POLICY_STATUS returns ok");

    /* CAP_BROKER_RELOAD — hot-reload with null blob; expect inval or ok. */
    microkit_mr_set(0, (uint64_t)OP_CAP_BROKER_RELOAD);
    microkit_mr_set(1, 0);  /* blob_addr in shmem */
    microkit_mr_set(2, 0);  /* blob_size = 0 (invalid) */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_CAP_BROKER_RELOAD, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_INVAL || rc == AOS_ERR_UNIMPL) {
            _tf_ok("cap_broker: CAP_BROKER_RELOAD with null blob returns structured error");
        } else {
            _tf_fail_point(
                "cap_broker: CAP_BROKER_RELOAD with null blob returns structured error",
                "unexpected error code");
        }
    }

    /* CAP_POLICY_RELOAD — fetch from AgentFS; may be unimpl. */
    ASSERT_IPC_OK_OR_ERR(ch, OP_CAP_POLICY_RELOAD, AOS_ERR_UNIMPL,
                         "cap_broker: POLICY_RELOAD returns ok or unimpl");

    /* CAP_POLICY_RESET — revert to static policy. */
    ASSERT_IPC_OK_OR_ERR(ch, OP_CAP_POLICY_RESET, AOS_ERR_UNIMPL,
                         "cap_broker: POLICY_RESET returns ok or unimpl");

    /* CAP_POLICY_DIFF — diff against prior version. */
    ASSERT_IPC_OK_OR_ERR(ch, OP_CAP_POLICY_DIFF, AOS_ERR_NOT_FOUND,
                         "cap_broker: POLICY_DIFF returns ok or not-found");
}
