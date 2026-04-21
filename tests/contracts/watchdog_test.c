/*
 * watchdog_test.c — contract tests for the Watchdog PD
 *
 * Covered opcodes:
 *   OP_WD_REGISTER   (0x50) — register a slot for heartbeat monitoring
 *   OP_WD_HEARTBEAT  (0x51) — update heartbeat tick for a slot
 *   OP_WD_STATUS     (0x52) — query monitoring state and ticks_remaining
 *   OP_WD_UNREGISTER (0x53) — stop monitoring a slot
 *   OP_WD_FREEZE     (0x54) — suspend monitoring during hot-reload
 *   OP_WD_RESUME     (0x55) — resume monitoring after hot-reload
 *
 * Channel: CH_WATCHDOG_CTRL (56) from the controller's perspective.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_watchdog_tests(microkit_channel ch)
{
    TEST_SECTION("watchdog");

    /* REGISTER — register slot_id=0 with a 100-tick heartbeat window. */
    microkit_mr_set(0, (uint64_t)OP_WD_REGISTER);
    microkit_mr_set(1, 0);    /* slot_id */
    microkit_mr_set(2, 100);  /* heartbeat_ticks */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_WD_REGISTER, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == (uint64_t)WD_OK || rc == (uint64_t)WD_ERR_FULL) {
            _tf_ok("watchdog: REGISTER slot 0 returns WD_OK or WD_ERR_FULL");
        } else {
            _tf_fail_point("watchdog: REGISTER slot 0 returns WD_OK or WD_ERR_FULL",
                           "unexpected error code");
        }
    }

    /* STATUS — query state for slot_id=0. */
    microkit_mr_set(0, (uint64_t)OP_WD_STATUS);
    microkit_mr_set(1, 0);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_WD_STATUS, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == (uint64_t)WD_OK || rc == (uint64_t)WD_ERR_NOENT) {
            _tf_ok("watchdog: STATUS slot 0 returns WD_OK or WD_ERR_NOENT");
        } else {
            _tf_fail_point("watchdog: STATUS slot 0 returns WD_OK or WD_ERR_NOENT",
                           "unexpected error code");
        }
    }

    /* HEARTBEAT — update tick for slot_id=0. */
    microkit_mr_set(0, (uint64_t)OP_WD_HEARTBEAT);
    microkit_mr_set(1, 0);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_WD_HEARTBEAT, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == (uint64_t)WD_OK || rc == (uint64_t)WD_ERR_NOENT) {
            _tf_ok("watchdog: HEARTBEAT slot 0 returns WD_OK or WD_ERR_NOENT");
        } else {
            _tf_fail_point("watchdog: HEARTBEAT slot 0 returns WD_OK or WD_ERR_NOENT",
                           "unexpected error code");
        }
    }

    /* FREEZE — suspend monitoring for hot-reload. */
    microkit_mr_set(0, (uint64_t)OP_WD_FREEZE);
    microkit_mr_set(1, 0);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_WD_FREEZE, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == (uint64_t)WD_OK || rc == (uint64_t)WD_ERR_NOENT) {
            _tf_ok("watchdog: FREEZE slot 0 returns WD_OK or WD_ERR_NOENT");
        } else {
            _tf_fail_point("watchdog: FREEZE slot 0 returns WD_OK or WD_ERR_NOENT",
                           "unexpected error code");
        }
    }

    /* RESUME — resume monitoring after hot-reload. */
    microkit_mr_set(0, (uint64_t)OP_WD_RESUME);
    microkit_mr_set(1, 0);   /* slot_id */
    microkit_mr_set(2, 0);   /* new_module_hash_lo */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_WD_RESUME, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == (uint64_t)WD_OK || rc == (uint64_t)WD_ERR_NOENT) {
            _tf_ok("watchdog: RESUME slot 0 returns WD_OK or WD_ERR_NOENT");
        } else {
            _tf_fail_point("watchdog: RESUME slot 0 returns WD_OK or WD_ERR_NOENT",
                           "unexpected error code");
        }
    }

    /* UNREGISTER — stop monitoring slot_id=0. */
    microkit_mr_set(0, (uint64_t)OP_WD_UNREGISTER);
    microkit_mr_set(1, 0);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_WD_UNREGISTER, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == (uint64_t)WD_OK || rc == (uint64_t)WD_ERR_NOENT) {
            _tf_ok("watchdog: UNREGISTER slot 0 returns WD_OK or WD_ERR_NOENT");
        } else {
            _tf_fail_point("watchdog: UNREGISTER slot 0 returns WD_OK or WD_ERR_NOENT",
                           "unexpected error code");
        }
    }
}
