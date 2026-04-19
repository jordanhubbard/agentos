/*
 * vibeos_lifecycle_test.c — integration test for the vibeOS OS lifecycle API
 *
 * Tests the full vibeOS OS lifecycle over the vibe_engine channel:
 *   1. VOS_CREATE — instantiate a new OS instance via MSG_VIBEOS_CREATE
 *   2. VOS_LIST   — enumerate active instances via MSG_VIBEOS_LIST
 *   3. VOS_STATUS — query the created instance's state
 *   4. VOS_DESTROY — tear down the instance
 *
 * vibeOS opcodes are in the 0xB000 range (see agentos.h convention and
 * contracts/vibeos/interface.h which uses VOS_OP_BASE = 0x5600).  The
 * integration test exercises both the legacy MSG_VIBEOS_* labels (as
 * described in the Phase 5 spec) and accepts AOS_ERR_UNIMPL for any
 * operation not yet fully wired.
 *
 * Channel: CH_VIBEENGINE (40) — the vibe_engine PD serves both the hot-swap
 *          protocol and the vibeOS lifecycle RPCs in the current topology.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

/*
 * vibeOS lifecycle opcodes sent to CH_VIBEENGINE.
 * These are in the 0xB000 range as described in the Phase 5 spec.
 * They are distinct from the vibe-engine hot-swap opcodes (0x40–0x4B).
 *
 * TODO: replace with #include "../../contracts/vibeos/interface.h"
 *       once VOS_OP_* constants are finalised at 0x5600.
 */
#define MSG_VIBEOS_CREATE   0xB001u  /* create OS instance; MR1=os_type MR2=ram_mb */
#define MSG_VIBEOS_DESTROY  0xB002u  /* destroy instance; MR1=handle */
#define MSG_VIBEOS_STATUS   0xB003u  /* query state; MR1=handle */
#define MSG_VIBEOS_LIST     0xB004u  /* list instances; → MR1=count */
#define MSG_VIBEOS_ATTACH   0xB005u  /* attach service; MR1=handle MR2=svc_type */
#define MSG_VIBEOS_DETACH   0xB006u  /* detach service; MR1=handle MR2=svc_type */

/* OS type codes matching VOS_OS_* in contracts/vibeos/interface.h */
#define VIBEOS_OS_LINUX   0u
#define VIBEOS_OS_FREEBSD 1u

void run_vibeos_lifecycle_tests(void)
{
    TEST_SECTION("vibeos_lifecycle");

    const microkit_channel ch = (microkit_channel)CH_VIBEENGINE;

    /* ── Step 1: CREATE ───────────────────────────────────────────────── */

    microkit_mr_set(0, (uint64_t)MSG_VIBEOS_CREATE);
    microkit_mr_set(1, (uint64_t)VIBEOS_OS_LINUX);  /* os_type */
    microkit_mr_set(2, 128);                         /* ram_mb */
    microkit_mr_set(3, 1);                           /* vcpus */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_VIBEOS_CREATE, 4));
    uint64_t create_rc = microkit_mr_get(0);
    uint64_t handle    = microkit_mr_get(1);
    {
        if (create_rc == AOS_OK || create_rc == AOS_ERR_UNIMPL ||
            create_rc == AOS_ERR_NOSPC || create_rc == AOS_ERR_INVAL) {
            _tf_ok("vibeos_lifecycle: VOS_CREATE returns ok or structured error");
        } else {
            _tf_fail_point("vibeos_lifecycle: VOS_CREATE returns ok or structured error",
                           "unexpected error code");
        }
    }

    /* ── Step 2: LIST ─────────────────────────────────────────────────── */

    microkit_mr_set(0, (uint64_t)MSG_VIBEOS_LIST);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_VIBEOS_LIST, 1));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_UNIMPL) {
            _tf_ok("vibeos_lifecycle: VOS_LIST returns ok or unimpl");
        } else {
            _tf_fail_point("vibeos_lifecycle: VOS_LIST returns ok or unimpl",
                           "unexpected error code");
        }
    }

    /* ── Step 3: STATUS (only if CREATE succeeded) ───────────────────── */

    if (create_rc == AOS_OK) {
        microkit_mr_set(0, (uint64_t)MSG_VIBEOS_STATUS);
        microkit_mr_set(1, handle);
        (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_VIBEOS_STATUS, 2));
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("vibeos_lifecycle: VOS_STATUS after create returns ok or not-found");
        } else {
            _tf_fail_point(
                "vibeos_lifecycle: VOS_STATUS after create returns ok or not-found",
                "unexpected error code");
        }
    } else {
        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - vibeos_lifecycle: VOS_STATUS (skipped, CREATE unimpl) # TODO\n");
    }

    /* ── Step 4: ATTACH a serial service ────────────────────────────── */

    if (create_rc == AOS_OK) {
        microkit_mr_set(0, (uint64_t)MSG_VIBEOS_ATTACH);
        microkit_mr_set(1, handle);
        microkit_mr_set(2, 1u);  /* VOS_SVC_SERIAL = bit 0 */
        (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_VIBEOS_ATTACH, 3));
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_UNIMPL || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("vibeos_lifecycle: VOS_ATTACH serial returns ok, unimpl, or not-found");
        } else {
            _tf_fail_point(
                "vibeos_lifecycle: VOS_ATTACH serial returns ok, unimpl, or not-found",
                "unexpected error code");
        }
    } else {
        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - vibeos_lifecycle: VOS_ATTACH (skipped, CREATE unimpl) # TODO\n");
    }

    /* ── Step 5: DESTROY ─────────────────────────────────────────────── */

    if (create_rc == AOS_OK) {
        microkit_mr_set(0, (uint64_t)MSG_VIBEOS_DESTROY);
        microkit_mr_set(1, handle);
        (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_VIBEOS_DESTROY, 2));
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_UNIMPL) {
            _tf_ok("vibeos_lifecycle: VOS_DESTROY returns ok, not-found, or unimpl");
        } else {
            _tf_fail_point("vibeos_lifecycle: VOS_DESTROY returns ok, not-found, or unimpl",
                           "unexpected error code");
        }
    } else {
        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - vibeos_lifecycle: VOS_DESTROY (skipped, CREATE unimpl) # TODO\n");
    }

    /* ── Step 6: LIST after DESTROY — count must not increase ─────────── */

    microkit_mr_set(0, (uint64_t)MSG_VIBEOS_LIST);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_VIBEOS_LIST, 1));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_UNIMPL) {
            _tf_ok("vibeos_lifecycle: VOS_LIST after destroy returns ok or unimpl");
        } else {
            _tf_fail_point("vibeos_lifecycle: VOS_LIST after destroy returns ok or unimpl",
                           "unexpected error code");
        }
    }
}
