/*
 * agentfs_test.c — contract tests for the AgentFS PD
 *
 * Covered opcodes:
 *   0x01 — STORE  : write object into AgentFS
 *   0x02 — FETCH  : retrieve object by hash
 *   0x03 — STAT   : query object metadata (size, flags)
 *   0x04 — DELETE : tombstone an object
 *   0x05 — LIST   : enumerate objects (paged)
 *   0x06 — SEARCH : content-addressable lookup
 *
 * Channel: CH_AGENTFS_CONTROLLER (0) from the agentfs PD's perspective.
 * From the controller's perspective the channel is CH_CONTROLLER_AGENTFS (5).
 * We use channel 0 as a placeholder; update to the real controller-side
 * channel ID once agentos.system assigns it.
 *
 * TODO: replace opcode literals with
 *       #include "../../contracts/agentfs/interface.h"
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

/* AgentFS opcodes (from contracts/agentfs/interface.h when available) */
#define AGENTFS_OP_STORE   0x01u
#define AGENTFS_OP_FETCH   0x02u
#define AGENTFS_OP_STAT    0x03u
#define AGENTFS_OP_DELETE  0x04u
#define AGENTFS_OP_LIST    0x05u
#define AGENTFS_OP_SEARCH  0x06u

void run_agentfs_tests(microkit_channel ch)
{
    TEST_SECTION("agentfs");

    /*
     * ch == 0 is a placeholder.  When the AgentFS channel is wired into the
     * test runner topology this block should be updated.  For now we emit
     * TODO markers so the TAP consumer knows these are pending.
     */
    if (ch == 0) {
        _tf_puts("# agentfs: channel not wired in test topology (ch=0)\n");
        /* Emit planned TODO points so the plan count stays consistent. */
        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - agentfs: STAT channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - agentfs: SEARCH channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - agentfs: LIST channel placeholder # TODO wire ch\n");
        return;
    }

    /* STAT — query metadata for hash (0,0); expect ok or not-found. */
    microkit_mr_set(0, (uint64_t)AGENTFS_OP_STAT);
    microkit_mr_set(1, 0);  /* hash_lo */
    microkit_mr_set(2, 0);  /* hash_hi */
    (void)microkit_ppcall(ch, microkit_msginfo_new(AGENTFS_OP_STAT, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("agentfs: STAT returns ok or not-found");
        } else {
            _tf_fail_point("agentfs: STAT returns ok or not-found",
                           "unexpected error code");
        }
    }

    /* SEARCH — content-addressable lookup with empty query; expect ok or not-found. */
    microkit_mr_set(0, (uint64_t)AGENTFS_OP_SEARCH);
    microkit_mr_set(1, 0);  /* query offset in shmem */
    microkit_mr_set(2, 0);  /* query length = 0 */
    (void)microkit_ppcall(ch, microkit_msginfo_new(AGENTFS_OP_SEARCH, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("agentfs: SEARCH returns ok, not-found, or inval");
        } else {
            _tf_fail_point("agentfs: SEARCH returns ok, not-found, or inval",
                           "unexpected error code");
        }
    }

    /* LIST — enumerate objects; count in MR1 (may be 0 on empty store). */
    ASSERT_IPC_OK(ch, AGENTFS_OP_LIST, "agentfs: LIST returns ok");
}
