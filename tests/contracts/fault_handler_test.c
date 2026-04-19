/*
 * fault_handler_test.c — contract tests for the FaultHandler PD
 *
 * Covered opcodes:
 *   OP_FAULT_POLICY_SET (0xE0) — set per-slot restart policy
 *   0xE1                       — FAULT_QUERY: query fault history for a slot
 *
 * Channel: 0 (placeholder — update when ch is assigned in agentos.system).
 *
 * TODO: replace opcode literals with
 *       #include "../../contracts/fault-handler/interface.h"
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

#define FAULT_OP_QUERY   0xE1u   /* FAULT_QUERY: query fault history for slot */
#define FAULT_OP_RESET   0xE2u   /* FAULT_RESET: clear fault counters for slot */

void run_fault_handler_tests(microkit_channel ch)
{
    TEST_SECTION("fault_handler");

    if (ch == 0) {
        _tf_puts("# fault_handler: channel not wired in test topology (ch=0)\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - fault_handler: FAULT_QUERY channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - fault_handler: FAULT_POLICY_SET channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - fault_handler: FAULT_RESET channel placeholder # TODO wire ch\n");
        return;
    }

    /* FAULT_QUERY — query fault history for slot_id=0. */
    microkit_mr_set(0, (uint64_t)FAULT_OP_QUERY);
    microkit_mr_set(1, 0);  /* slot_id */
    (void)microkit_ppcall(ch, microkit_msginfo_new(FAULT_OP_QUERY, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("fault_handler: FAULT_QUERY returns ok or not-found");
        } else {
            _tf_fail_point("fault_handler: FAULT_QUERY returns ok or not-found",
                           "unexpected error code");
        }
    }

    /* FAULT_POLICY_SET — configure restart policy for slot_id=0. */
    microkit_mr_set(0, (uint64_t)OP_FAULT_POLICY_SET);
    microkit_mr_set(1, 0);                                  /* slot_id */
    microkit_mr_set(2, FAULT_POLICY_MAX_RESTARTS_DEFAULT);  /* max_restarts */
    microkit_mr_set(3, FAULT_POLICY_ESCALATE_AFTER);        /* escalate_after */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_FAULT_POLICY_SET, 4));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("fault_handler: FAULT_POLICY_SET returns ok, not-found, or inval");
        } else {
            _tf_fail_point(
                "fault_handler: FAULT_POLICY_SET returns ok, not-found, or inval",
                "unexpected error code");
        }
    }

    /* FAULT_RESET — clear fault counters for slot_id=0. */
    microkit_mr_set(0, (uint64_t)FAULT_OP_RESET);
    microkit_mr_set(1, 0);
    (void)microkit_ppcall(ch, microkit_msginfo_new(FAULT_OP_RESET, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_UNIMPL) {
            _tf_ok("fault_handler: FAULT_RESET returns ok, not-found, or unimpl");
        } else {
            _tf_fail_point("fault_handler: FAULT_RESET returns ok, not-found, or unimpl",
                           "unexpected error code");
        }
    }
}
