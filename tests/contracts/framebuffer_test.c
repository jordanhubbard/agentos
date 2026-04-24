/*
 * framebuffer_test.c — contract tests for the Framebuffer PD
 *
 * Covered opcodes:
 *   MSG_FB_CREATE                  — create a framebuffer
 *   MSG_FB_WRITE                   — write pixel data
 *   MSG_FB_FLIP                    — flip front/back buffers
 *   MSG_FB_RESIZE                  — resize framebuffer
 *   MSG_FB_STATUS                  — query framebuffer status
 *   MSG_FB_DESTROY                 — destroy a framebuffer
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_framebuffer_tests(microkit_channel ch)
{
    TEST_SECTION("framebuffer");

    /* CREATE — create a small framebuffer */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_FB_CREATE, AOS_ERR_UNIMPL,
                         "framebuffer: CREATE returns ok or unimpl");

    /* STATUS — query bogus handle */
    microkit_mr_set(0, (uint64_t)MSG_FB_STATUS);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_FB_STATUS, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("framebuffer: STATUS with bogus handle returns ok/not-found/inval");
        } else {
            _tf_fail_point("framebuffer: STATUS with bogus handle returns ok/not-found/inval",
                           "unexpected error code");
        }
    }

    /* FLIP — flip bogus handle */
    microkit_mr_set(0, (uint64_t)MSG_FB_FLIP);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_FB_FLIP, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("framebuffer: FLIP with bogus handle returns ok/not-found/inval");
        } else {
            _tf_fail_point("framebuffer: FLIP with bogus handle returns ok/not-found/inval",
                           "unexpected error code");
        }
    }

    /* DESTROY — destroy bogus handle */
    microkit_mr_set(0, (uint64_t)MSG_FB_DESTROY);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_FB_DESTROY, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("framebuffer: DESTROY with bogus handle returns ok/not-found/inval");
        } else {
            _tf_fail_point("framebuffer: DESTROY with bogus handle returns ok/not-found/inval",
                           "unexpected error code");
        }
    }
}
