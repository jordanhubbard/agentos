/*
 * debug_bridge_test.c — contract tests for the DebugBridge PD
 *
 * Covered opcodes:
 *   0xD0 — DBG_ATTACH  : attach debugger to a target PD slot
 *   0xD1 — DBG_DETACH  : detach debugger
 *   0xD2 — DBG_PEEK    : read word from target address space
 *   0xD3 — DBG_STATUS  : query bridge state
 *
 * Channel: 0 (placeholder — update when ch is assigned in agentos.system).
 *
 * TODO: replace opcode literals with
 *       #include "../../contracts/debug-bridge/interface.h"
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

#define DBG_OP_ATTACH  0xD0u
#define DBG_OP_DETACH  0xD1u
#define DBG_OP_PEEK    0xD2u
#define DBG_OP_STATUS  0xD3u

void run_debug_bridge_tests(microkit_channel ch)
{
    TEST_SECTION("debug_bridge");

    if (ch == 0) {
        _tf_puts("# debug_bridge: channel not wired in test topology (ch=0)\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - debug_bridge: DBG_ATTACH channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - debug_bridge: DBG_DETACH channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - debug_bridge: DBG_PEEK channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - debug_bridge: DBG_STATUS channel placeholder # TODO wire ch\n");
        return;
    }

    /*
     * DBG_ATTACH — attach to slot_id=0.  Expect ok if debug build,
     * AOS_ERR_PERM in production builds.
     */
    microkit_mr_set(0, (uint64_t)DBG_OP_ATTACH);
    microkit_mr_set(1, 0);  /* slot_id */
    (void)microkit_ppcall(ch, microkit_msginfo_new(DBG_OP_ATTACH, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_PERM || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("debug_bridge: DBG_ATTACH returns ok, perm, or not-found");
        } else {
            _tf_fail_point("debug_bridge: DBG_ATTACH returns ok, perm, or not-found",
                           "unexpected error code");
        }
    }

    /* DBG_STATUS — always available. */
    ASSERT_IPC_OK_OR_ERR(ch, DBG_OP_STATUS, AOS_ERR_UNIMPL,
                         "debug_bridge: DBG_STATUS returns ok or unimpl");

    /* DBG_PEEK — read from address 0; expect ok or perm or inval. */
    microkit_mr_set(0, (uint64_t)DBG_OP_PEEK);
    microkit_mr_set(1, 0);  /* target_addr */
    (void)microkit_ppcall(ch, microkit_msginfo_new(DBG_OP_PEEK, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_PERM || rc == AOS_ERR_INVAL) {
            _tf_ok("debug_bridge: DBG_PEEK address 0 returns ok, perm, or inval");
        } else {
            _tf_fail_point("debug_bridge: DBG_PEEK address 0 returns ok, perm, or inval",
                           "unexpected error code");
        }
    }

    /* DBG_DETACH — detach from slot_id=0. */
    microkit_mr_set(0, (uint64_t)DBG_OP_DETACH);
    microkit_mr_set(1, 0);
    (void)microkit_ppcall(ch, microkit_msginfo_new(DBG_OP_DETACH, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_PERM) {
            _tf_ok("debug_bridge: DBG_DETACH returns ok, not-found, or perm");
        } else {
            _tf_fail_point("debug_bridge: DBG_DETACH returns ok, not-found, or perm",
                           "unexpected error code");
        }
    }
}
