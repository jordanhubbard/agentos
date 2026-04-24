/*
 * auth_server_test.c — contract tests for the AuthServer PD
 *
 * Covered opcodes:
 *   OP_AUTH_LOGIN                  — authenticate a client
 *   OP_AUTH_VERIFY                 — verify a token
 *   OP_AUTH_REVOKE                 — revoke a token
 *   OP_AUTH_ADDUSER                — add a new user
 *   OP_AUTH_STATUS                 — query auth service status
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_auth_server_tests(microkit_channel ch)
{
    TEST_SECTION("auth_server");

    /* STATUS — basic health check */
    ASSERT_IPC_OK(ch, OP_AUTH_STATUS, "auth_server: STATUS returns ok");

    /* VERIFY — verify a bogus token; expect PERM or NOT_FOUND */
    microkit_mr_set(0, (uint64_t)OP_AUTH_VERIFY);
    microkit_mr_set(1, 0xDEADu);  /* bogus token */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_AUTH_VERIFY, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_ERR_PERM || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("auth_server: VERIFY with bad token returns perm/not-found/inval");
        } else {
            _tf_fail_point("auth_server: VERIFY with bad token returns perm/not-found/inval",
                           "unexpected error code");
        }
    }

    /* REVOKE — revoke a bogus token */
    microkit_mr_set(0, (uint64_t)OP_AUTH_REVOKE);
    microkit_mr_set(1, 0xDEADu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_AUTH_REVOKE, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_PERM || rc == AOS_ERR_INVAL) {
            _tf_ok("auth_server: REVOKE with bad token returns not-found/perm/inval");
        } else {
            _tf_fail_point("auth_server: REVOKE with bad token returns not-found/perm/inval",
                           "unexpected error code");
        }
    }

    /* LOGIN — without valid credentials in shmem; expect INVAL or UNIMPL */
    ASSERT_IPC_OK_OR_ERR(ch, OP_AUTH_LOGIN, AOS_ERR_INVAL,
                         "auth_server: LOGIN without creds returns inval or ok");

    /* ADDUSER — expect PERM or UNIMPL (requires admin privileges) */
    ASSERT_IPC_OK_OR_ERR(ch, OP_AUTH_ADDUSER, AOS_ERR_PERM,
                         "auth_server: ADDUSER returns perm or ok");
}
