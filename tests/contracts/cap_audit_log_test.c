/*
 * cap_audit_log_test.c — contract tests for the CapAuditLog PD
 *
 * Covered opcodes:
 *   OP_CAP_LOG        (0x50) — log a grant/revoke event
 *   OP_CAP_LOG_STATUS (0x51) — query ring buffer status
 *   OP_CAP_LOG_DUMP   (0x52) — read entries from the ring
 *   OP_CAP_ATTEST     (0x53) — generate signed capability attestation report
 *
 * Channel: CH_CAP_AUDIT_CTRL (57) from the controller's perspective.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_cap_audit_log_tests(microkit_channel ch)
{
    TEST_SECTION("cap_audit_log");

    /* STATUS — ring buffer status must always be available. */
    ASSERT_IPC_OK(ch, OP_CAP_LOG_STATUS, "cap_audit_log: STATUS returns ok");

    /* LOG — log a grant event for agent_id=0, caps=0. */
    microkit_mr_set(0, (uint64_t)OP_CAP_LOG);
    microkit_mr_set(1, (uint64_t)CAP_EVENT_GRANT);  /* event_type */
    microkit_mr_set(2, 0);                           /* agent_id */
    microkit_mr_set(3, 0);                           /* caps_mask */
    microkit_mr_set(4, 0);                           /* slot_id */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_CAP_LOG, 5));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOSPC) {
            _tf_ok("cap_audit_log: LOG grant event returns ok or nospc");
        } else {
            _tf_fail_point("cap_audit_log: LOG grant event returns ok or nospc",
                           "unexpected error code");
        }
    }

    /* LOG — log a revoke event. */
    microkit_mr_set(0, (uint64_t)OP_CAP_LOG);
    microkit_mr_set(1, (uint64_t)CAP_EVENT_REVOKE);
    microkit_mr_set(2, 0);
    microkit_mr_set(3, 0);
    microkit_mr_set(4, 0);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_CAP_LOG, 5));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOSPC) {
            _tf_ok("cap_audit_log: LOG revoke event returns ok or nospc");
        } else {
            _tf_fail_point("cap_audit_log: LOG revoke event returns ok or nospc",
                           "unexpected error code");
        }
    }

    /* DUMP — read entries from the ring; count in MR1. */
    microkit_mr_set(0, (uint64_t)OP_CAP_LOG_DUMP);
    microkit_mr_set(1, 0);   /* start offset */
    microkit_mr_set(2, 16);  /* max entries */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_CAP_LOG_DUMP, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("cap_audit_log: DUMP returns ok or not-found");
        } else {
            _tf_fail_point("cap_audit_log: DUMP returns ok or not-found",
                           "unexpected error code");
        }
    }

    /* ATTEST — generate attestation report; may be unimpl in Phase 5. */
    ASSERT_IPC_OK_OR_ERR(ch, OP_CAP_ATTEST, AOS_ERR_UNIMPL,
                         "cap_audit_log: ATTEST returns ok or unimpl");
}
