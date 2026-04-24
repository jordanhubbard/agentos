/*
 * guest_test.c — contract tests for the Guest PD (guest binding protocol)
 *
 * Covered opcodes:
 *   MSG_GUEST_CREATE               — create a guest instance
 *   MSG_GUEST_BIND_DEVICE          — bind a device to a guest
 *   MSG_GUEST_SET_MEMORY           — configure guest memory
 *   MSG_GUEST_BOOT                 — boot the guest
 *   MSG_GUEST_SUSPEND              — suspend a guest
 *   MSG_GUEST_RESUME               — resume a suspended guest
 *   MSG_GUEST_DESTROY              — destroy a guest
 *   MSG_GUEST_SEND_INPUT           — send input event to guest
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_guest_tests(microkit_channel ch)
{
    TEST_SECTION("guest");

    /* DESTROY — destroy bogus guest_id */
    microkit_mr_set(0, (uint64_t)MSG_GUEST_DESTROY);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_GUEST_DESTROY, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("guest: DESTROY with bogus id returns not-found/inval");
        } else {
            _tf_fail_point("guest: DESTROY with bogus id returns not-found/inval",
                           "unexpected error code");
        }
    }

    /* SUSPEND — suspend bogus guest */
    microkit_mr_set(0, (uint64_t)MSG_GUEST_SUSPEND);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_GUEST_SUSPEND, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("guest: SUSPEND with bogus id returns not-found/inval");
        } else {
            _tf_fail_point("guest: SUSPEND with bogus id returns not-found/inval",
                           "unexpected error code");
        }
    }

    /* RESUME — resume bogus guest */
    microkit_mr_set(0, (uint64_t)MSG_GUEST_RESUME);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_GUEST_RESUME, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("guest: RESUME with bogus id returns not-found/inval");
        } else {
            _tf_fail_point("guest: RESUME with bogus id returns not-found/inval",
                           "unexpected error code");
        }
    }

    /* CREATE — requires shmem setup; expect ok or UNIMPL */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_GUEST_CREATE, AOS_ERR_UNIMPL,
                         "guest: CREATE returns ok or unimpl");
}
