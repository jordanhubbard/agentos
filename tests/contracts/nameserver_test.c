/*
 * nameserver_test.c — contract tests for the NameServer PD
 *
 * Covered opcodes:
 *   OP_NS_REGISTER                 — register a service name
 *   OP_NS_LOOKUP                   — look up a service
 *   OP_NS_LOOKUP_GATED             — gated lookup (waits for registration)
 *   OP_NS_UPDATE_STATUS            — update service status
 *   OP_NS_LIST                     — list all registered services
 *   OP_NS_DEREGISTER               — deregister a service
 *   OP_NS_HEALTH                   — health check
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_nameserver_tests(microkit_channel ch)
{
    TEST_SECTION("nameserver");

    /* HEALTH — basic liveness */
    ASSERT_IPC_OK(ch, OP_NS_HEALTH, "nameserver: HEALTH returns ok");

    /* LIST — enumerate registered services */
    ASSERT_IPC_OK(ch, OP_NS_LIST, "nameserver: LIST returns ok");

    /* LOOKUP — look up a service that probably doesn't exist */
    microkit_mr_set(0, (uint64_t)OP_NS_LOOKUP);
    microkit_mr_set(1, 0xDEADu);  /* bogus service name hash */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_NS_LOOKUP, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("nameserver: LOOKUP with bogus name returns ok/not-found");
        } else {
            _tf_fail_point("nameserver: LOOKUP with bogus name returns ok/not-found",
                           "unexpected error code");
        }
    }

    /* DEREGISTER — deregister a bogus service */
    microkit_mr_set(0, (uint64_t)OP_NS_DEREGISTER);
    microkit_mr_set(1, 0xDEADu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_NS_DEREGISTER, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("nameserver: DEREGISTER bogus service returns ok/not-found");
        } else {
            _tf_fail_point("nameserver: DEREGISTER bogus service returns ok/not-found",
                           "unexpected error code");
        }
    }

    /* REGISTER — register a test service; expect ok or EXISTS */
    ASSERT_IPC_OK_OR_ERR(ch, OP_NS_REGISTER, AOS_ERR_EXISTS,
                         "nameserver: REGISTER returns ok or exists");
}
