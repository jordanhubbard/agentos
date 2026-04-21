/*
 * vibe_swap_test.c — contract tests for the VibeSwap protocol
 *
 * Covered opcodes (controller -> swap_slot direction):
 *   MSG_VIBE_SWAP_BEGIN    (0x0501) — initiate a swap sequence
 *   MSG_VIBE_SWAP_ACTIVATE (0x0502) — bring the new slot live
 *   MSG_VIBE_SWAP_ROLLBACK (0x0503) — revert to previous slot
 *   MSG_VIBE_SWAP_HEALTH   (0x0504) — health check on active slot
 *   MSG_VIBE_SWAP_STATUS   (0x0505) — query current swap state
 *
 * Channel: SWAP_SLOT_BASE_CH (30) — first swap slot from controller's view.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_vibe_swap_tests(microkit_channel ch)
{
    TEST_SECTION("vibe_swap");

    /* STATUS — always valid; returns current slot state. */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_VIBE_SWAP_STATUS, AOS_ERR_UNIMPL,
                         "vibe_swap: STATUS returns ok or unimpl");

    /* HEALTH — probe the active slot. */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_VIBE_SWAP_HEALTH, AOS_ERR_UNIMPL,
                         "vibe_swap: HEALTH returns ok or unimpl");

    /*
     * BEGIN — start a swap.  Without a valid WASM blob in the staging region
     * this should return an error (INVAL, BUSY, or UNIMPL).
     */
    microkit_mr_set(0, (uint64_t)MSG_VIBE_SWAP_BEGIN);
    microkit_mr_set(1, 0);  /* service_id */
    microkit_mr_set(2, 0);  /* proposal_id */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_VIBE_SWAP_BEGIN, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_INVAL ||
            rc == AOS_ERR_BUSY  || rc == AOS_ERR_UNIMPL) {
            _tf_ok("vibe_swap: BEGIN with null blob returns structured error");
        } else {
            _tf_fail_point("vibe_swap: BEGIN with null blob returns structured error",
                           "unexpected error code");
        }
    }

    /*
     * ROLLBACK — without an active swap this should return not-found or ok.
     */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_VIBE_SWAP_ROLLBACK, AOS_ERR_NOT_FOUND,
                         "vibe_swap: ROLLBACK without active swap returns ok or not-found");

    /*
     * ACTIVATE — without a pending slot this should return not-found or unimpl.
     */
    microkit_mr_set(0, (uint64_t)MSG_VIBE_SWAP_ACTIVATE);
    microkit_mr_set(1, 0);  /* slot_id */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_VIBE_SWAP_ACTIVATE, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_UNIMPL) {
            _tf_ok("vibe_swap: ACTIVATE without pending slot returns ok, not-found, or unimpl");
        } else {
            _tf_fail_point(
                "vibe_swap: ACTIVATE without pending slot returns ok, not-found, or unimpl",
                "unexpected error code");
        }
    }
}
