/*
 * http_svc_test.c — contract tests for the HTTP Service PD
 *
 * Covered opcodes:
 *   OP_HTTP_REGISTER               — register an HTTP endpoint
 *   OP_HTTP_UNREGISTER             — unregister an endpoint
 *   OP_HTTP_DISPATCH               — dispatch an HTTP request
 *   OP_HTTP_LIST                   — list registered endpoints
 *   OP_HTTP_HEALTH                 — health check
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_http_svc_tests(microkit_channel ch)
{
    TEST_SECTION("http_svc");

    /* HEALTH — basic liveness check */
    ASSERT_IPC_OK(ch, OP_HTTP_HEALTH, "http_svc: HEALTH returns ok");

    /* LIST — enumerate registered endpoints */
    ASSERT_IPC_OK(ch, OP_HTTP_LIST, "http_svc: LIST returns ok");

    /* UNREGISTER — unregister bogus endpoint */
    microkit_mr_set(0, (uint64_t)OP_HTTP_UNREGISTER);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_HTTP_UNREGISTER, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("http_svc: UNREGISTER with bogus id returns not-found/inval");
        } else {
            _tf_fail_point("http_svc: UNREGISTER with bogus id returns not-found/inval",
                           "unexpected error code");
        }
    }

    /* REGISTER — expect ok or UNIMPL */
    ASSERT_IPC_OK_OR_ERR(ch, OP_HTTP_REGISTER, AOS_ERR_UNIMPL,
                         "http_svc: REGISTER returns ok or unimpl");

    /* DISPATCH — expect INVAL without proper request in shmem */
    ASSERT_IPC_OK_OR_ERR(ch, OP_HTTP_DISPATCH, AOS_ERR_INVAL,
                         "http_svc: DISPATCH without request returns inval or ok");
}
