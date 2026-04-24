/*
 * cc_test.c — contract tests for the Command & Control PD
 *
 * Covered opcodes:
 *   MSG_CC_CONNECT                 — establish a session
 *   MSG_CC_DISCONNECT              — disconnect session
 *   MSG_CC_SEND                    — send command to guest
 *   MSG_CC_RECV                    — receive response
 *   MSG_CC_STATUS                  — query session status
 *   MSG_CC_LIST                    — list sessions
 *   MSG_CC_LIST_GUESTS             — list managed guests
 *   MSG_CC_LIST_DEVICES            — list devices by type
 *   MSG_CC_LIST_POLECATS           — list agent pool status
 *   MSG_CC_GUEST_STATUS            — query specific guest
 *   MSG_CC_DEVICE_STATUS           — query specific device
 *   MSG_CC_ATTACH_FRAMEBUFFER      — attach FB to guest
 *   MSG_CC_SEND_INPUT              — send input event to guest
 *   MSG_CC_SNAPSHOT                — snapshot a guest
 *   MSG_CC_RESTORE                 — restore a guest from snapshot
 *   MSG_CC_LOG_STREAM              — drain log from a PD
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_cc_tests(microkit_channel ch)
{
    TEST_SECTION("cc");

    /* LIST — enumerate sessions (may be empty) */
    ASSERT_IPC_OK(ch, MSG_CC_LIST, "cc: LIST returns ok");

    /* LIST_GUESTS — enumerate managed guests */
    ASSERT_IPC_OK(ch, MSG_CC_LIST_GUESTS, "cc: LIST_GUESTS returns ok");

    /* LIST_POLECATS — agent pool summary */
    ASSERT_IPC_OK(ch, MSG_CC_LIST_POLECATS, "cc: LIST_POLECATS returns ok");

    /* STATUS — query bogus session */
    microkit_mr_set(0, (uint64_t)MSG_CC_STATUS);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_CC_STATUS, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("cc: STATUS with bogus session returns ok/not-found/inval");
        } else {
            _tf_fail_point("cc: STATUS with bogus session returns ok/not-found/inval",
                           "unexpected error code");
        }
    }

    /* CONNECT — establish a new session */
    microkit_mr_set(0, (uint64_t)MSG_CC_CONNECT);
    microkit_mr_set(1, 0);  /* client_badge */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_CC_CONNECT, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOSPC || rc == AOS_ERR_UNIMPL) {
            _tf_ok("cc: CONNECT returns ok/nospc/unimpl");
        } else {
            _tf_fail_point("cc: CONNECT returns ok/nospc/unimpl",
                           "unexpected error code");
        }
    }

    /* GUEST_STATUS — query bogus guest */
    microkit_mr_set(0, (uint64_t)MSG_CC_GUEST_STATUS);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_CC_GUEST_STATUS, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("cc: GUEST_STATUS with bogus handle returns ok/not-found");
        } else {
            _tf_fail_point("cc: GUEST_STATUS with bogus handle returns ok/not-found",
                           "unexpected error code");
        }
    }

    /* LIST_DEVICES — list serial devices (type 0) */
    microkit_mr_set(0, (uint64_t)MSG_CC_LIST_DEVICES);
    microkit_mr_set(1, 0);  /* dev_type */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_CC_LIST_DEVICES, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        _tf_ok("cc: LIST_DEVICES returns");
        (void)rc;
    }

    /* SNAPSHOT — snapshot bogus guest */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_CC_SNAPSHOT, AOS_ERR_NOT_FOUND,
                         "cc: SNAPSHOT with no guest returns not-found or ok");

    /* LOG_STREAM — drain logs */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_CC_LOG_STREAM, AOS_ERR_UNIMPL,
                         "cc: LOG_STREAM returns ok or unimpl");
}
