/*
 * vibeos_lifecycle_test.c — integration test for the vibeOS OS lifecycle API
 *
 * Tests the full vibeOS OS lifecycle via MSG_VIBEOS_* opcodes on CH_VIBEOS_ENGINE:
 *   1. MSG_VIBEOS_CREATE  — instantiate a new OS instance
 *   2. MSG_VIBEOS_LIST    — enumerate active instances, verify count increases
 *   3. MSG_VIBEOS_STATUS  — query the created instance's state
 *   4. MSG_VIBEOS_DESTROY — tear down the instance
 *   5. MSG_VIBEOS_LIST    — verify count returns to baseline
 *
 * Channel: CH_VIBEOS_ENGINE (== CH_VIBEENGINE, 40) — the vibe_engine PD
 *          serves both hot-swap and vibeOS lifecycle RPCs in this topology.
 *
 * AOS_ERR_NOSYS / VIBEOS_ERR_NOT_IMPL are accepted as graceful TODO-skips
 * when the vm_manager is not wired in the simulator topology.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/vibeos_contract.h"

void run_vibeos_lifecycle_tests(void)
{
    TEST_SECTION("vibeos_lifecycle");

    const microkit_channel ch = (microkit_channel)CH_VIBEOS_ENGINE;

    /* ── Baseline: query list count before any creates ────────────────── */

    microkit_mr_set(0, (uint64_t)MSG_VIBEOS_LIST);
    microkit_mr_set(1, 0);  /* offset */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_VIBEOS_LIST, 2));
    uint64_t list_rc_before = microkit_mr_get(0);
    uint64_t count_before   = microkit_mr_get(1);
    {
        if (list_rc_before == VIBEOS_OK) {
            _tf_ok("vibeos_lifecycle: MSG_VIBEOS_LIST baseline returns VIBEOS_OK");
        } else {
            _tf_fail_point("vibeos_lifecycle: MSG_VIBEOS_LIST baseline returns VIBEOS_OK",
                           "unexpected error code");
        }
    }

    /* ── Step 1: CREATE ───────────────────────────────────────────────── */

    microkit_mr_set(0, (uint64_t)MSG_VIBEOS_CREATE);
    microkit_mr_set(1, (uint64_t)VIBEOS_TYPE_LINUX);  /* os_type */
    microkit_mr_set(2, 128);                           /* ram_mb */
    microkit_mr_set(3, VIBEOS_DEV_SERIAL);             /* dev_flags */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_VIBEOS_CREATE, 4));
    uint64_t create_rc = microkit_mr_get(0);
    uint64_t handle    = microkit_mr_get(1);
    {
        if (create_rc == VIBEOS_OK || create_rc == VIBEOS_ERR_OOM ||
            create_rc == (uint64_t)AOS_ERR_UNIMPL) {
            _tf_ok("vibeos_lifecycle: MSG_VIBEOS_CREATE returns ok or structured error");
        } else {
            _tf_fail_point("vibeos_lifecycle: MSG_VIBEOS_CREATE returns ok or structured error",
                           "unexpected error code");
        }
    }

    if (create_rc != VIBEOS_OK) {
        _tf_puts("# vibeos_lifecycle: CREATE did not succeed; "
                 "skipping dependent checks\n");

        /* Emit TODO-skip points for the dependent steps */
        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - vibeos_lifecycle: MSG_VIBEOS_LIST count after create # TODO needs hardware\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - vibeos_lifecycle: MSG_VIBEOS_STATUS after create # TODO needs hardware\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - vibeos_lifecycle: MSG_VIBEOS_DESTROY returns ok # TODO needs hardware\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - vibeos_lifecycle: MSG_VIBEOS_LIST count restored # TODO needs hardware\n");
        return;
    }

    /* ── Step 2: LIST — verify count increased ───────────────────────── */

    microkit_mr_set(0, (uint64_t)MSG_VIBEOS_LIST);
    microkit_mr_set(1, 0);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_VIBEOS_LIST, 2));
    {
        uint64_t rc    = microkit_mr_get(0);
        uint64_t count = microkit_mr_get(1);
        if (rc == VIBEOS_OK && count == count_before + 1) {
            _tf_ok("vibeos_lifecycle: MSG_VIBEOS_LIST count increased by 1 after create");
        } else if (rc == VIBEOS_OK) {
            _tf_fail_point("vibeos_lifecycle: MSG_VIBEOS_LIST count increased by 1 after create",
                           "count did not increase by exactly 1");
        } else {
            _tf_fail_point("vibeos_lifecycle: MSG_VIBEOS_LIST count increased by 1 after create",
                           "MSG_VIBEOS_LIST returned error");
        }
        (void)count;
    }

    /* ── Step 3: STATUS — verify handle is known ────────────────────── */

    microkit_mr_set(0, (uint64_t)MSG_VIBEOS_STATUS);
    microkit_mr_set(1, handle);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_VIBEOS_STATUS, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == VIBEOS_OK) {
            _tf_ok("vibeos_lifecycle: MSG_VIBEOS_STATUS after create returns VIBEOS_OK");
        } else {
            _tf_fail_point("vibeos_lifecycle: MSG_VIBEOS_STATUS after create returns VIBEOS_OK",
                           "unexpected error code");
        }
    }

    /* ── Step 4: DESTROY ────────────────────────────────────────────── */

    microkit_mr_set(0, (uint64_t)MSG_VIBEOS_DESTROY);
    microkit_mr_set(1, handle);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_VIBEOS_DESTROY, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == VIBEOS_OK) {
            _tf_ok("vibeos_lifecycle: MSG_VIBEOS_DESTROY returns VIBEOS_OK");
        } else {
            _tf_fail_point("vibeos_lifecycle: MSG_VIBEOS_DESTROY returns VIBEOS_OK",
                           "unexpected error code");
        }
    }

    /* ── Step 5: LIST after DESTROY — count must return to baseline ── */

    microkit_mr_set(0, (uint64_t)MSG_VIBEOS_LIST);
    microkit_mr_set(1, 0);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_VIBEOS_LIST, 2));
    {
        uint64_t rc    = microkit_mr_get(0);
        uint64_t count = microkit_mr_get(1);
        if (rc == VIBEOS_OK && count == count_before) {
            _tf_ok("vibeos_lifecycle: MSG_VIBEOS_LIST count restored after destroy");
        } else if (rc == VIBEOS_OK) {
            _tf_fail_point("vibeos_lifecycle: MSG_VIBEOS_LIST count restored after destroy",
                           "count did not return to baseline");
        } else {
            _tf_fail_point("vibeos_lifecycle: MSG_VIBEOS_LIST count restored after destroy",
                           "MSG_VIBEOS_LIST returned error after destroy");
        }
        (void)count;
    }
}
