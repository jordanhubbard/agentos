/*
 * vibe_hotswap_test.c — integration test for the vibe-engine hot-swap pipeline
 *
 * Tests the hot-swap readiness of the vibe-engine:
 *   1. REGISTRY_STATUS — verify registry is accessible and returns stats
 *   2. REGISTRY_QUERY  — probe for a known-absent hash (zero hash)
 *   3. STATUS          — confirm engine is in a healthy state
 *   4. REPLAY          — trigger registry replay from AgentFS (if available)
 *
 * Channel: CH_VIBEENGINE (40) from the controller's perspective.
 *
 * This test does NOT write to the staging region or propose a real WASM module;
 * doing so would require a valid WASM binary and a mapped staging region, both
 * of which are hardware-dependent.  The test instead exercises the read-only
 * and probe paths that must work on any system where vibe_engine.c is running.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_vibe_hotswap_tests(void)
{
    TEST_SECTION("vibe_hotswap");

    const microkit_channel ch = (microkit_channel)CH_VIBEENGINE;

    /* ── 1. REGISTRY_STATUS ───────────────────────────────────────────── */

    microkit_mr_set(0, (uint64_t)OP_VIBE_REGISTRY_STATUS);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VIBE_REGISTRY_STATUS, 1));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK) {
            _tf_ok("vibe_hotswap: REGISTRY_STATUS returns ok");
        } else {
            _tf_fail_point("vibe_hotswap: REGISTRY_STATUS returns ok",
                           "vibe_engine did not return ok for REGISTRY_STATUS");
        }
    }

    /* ── 2. REGISTRY_QUERY — probe for zero hash ─────────────────────── */

    microkit_mr_set(0, (uint64_t)OP_VIBE_REGISTRY_QUERY);
    microkit_mr_set(1, 0);   /* hash_lo = 0 */
    microkit_mr_set(2, 0);   /* hash_hi = 0 */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VIBE_REGISTRY_QUERY, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("vibe_hotswap: REGISTRY_QUERY zero hash returns ok or not-found");
        } else {
            _tf_fail_point(
                "vibe_hotswap: REGISTRY_QUERY zero hash returns ok or not-found",
                "unexpected error code");
        }
    }

    /* ── 3. STATUS — engine health check ─────────────────────────────── */

    microkit_mr_set(0, (uint64_t)OP_VIBE_STATUS);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VIBE_STATUS, 1));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK) {
            _tf_ok("vibe_hotswap: STATUS returns ok");
        } else {
            _tf_fail_point("vibe_hotswap: STATUS returns ok",
                           "engine status check failed");
        }
    }

    /* ── 4. REPLAY — seed registry from AgentFS ──────────────────────── */

    microkit_mr_set(0, (uint64_t)OP_VIBE_REPLAY);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VIBE_REPLAY, 1));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_UNIMPL || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("vibe_hotswap: REPLAY returns ok, unimpl, or not-found");
        } else {
            _tf_fail_point("vibe_hotswap: REPLAY returns ok, unimpl, or not-found",
                           "unexpected error code");
        }
    }

    /* ── 5. REGISTRY_STATUS — verify registry is still consistent ──────── */

    microkit_mr_set(0, (uint64_t)OP_VIBE_REGISTRY_STATUS);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VIBE_REGISTRY_STATUS, 1));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK) {
            _tf_ok("vibe_hotswap: REGISTRY_STATUS still ok after REPLAY");
        } else {
            _tf_fail_point("vibe_hotswap: REGISTRY_STATUS still ok after REPLAY",
                           "registry corrupted after replay attempt");
        }
    }

    /* ── 6. HEALTH — probe active slot ──────────────────────────────── */

    microkit_mr_set(0, (uint64_t)OP_VIBE_HEALTH);
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VIBE_HEALTH, 1));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_UNIMPL || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("vibe_hotswap: HEALTH probe returns ok, unimpl, or not-found");
        } else {
            _tf_fail_point("vibe_hotswap: HEALTH probe returns ok, unimpl, or not-found",
                           "unexpected error code");
        }
    }

    /* ── 7. PROPOSE — propose a null WASM blob (must return INVAL) ─────── */

    microkit_mr_set(0, (uint64_t)OP_VIBE_PROPOSE);
    microkit_mr_set(1, 0);   /* service_id = 0 */
    microkit_mr_set(2, 0);   /* wasm_size = 0 (invalid) */
    microkit_mr_set(3, 0);   /* proposal_id = 0 */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VIBE_PROPOSE, 4));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_ERR_INVAL || rc == AOS_ERR_UNIMPL || rc == AOS_ERR_BUSY) {
            _tf_ok("vibe_hotswap: PROPOSE with null blob returns inval, unimpl, or busy");
        } else {
            _tf_fail_point(
                "vibe_hotswap: PROPOSE with null blob returns inval, unimpl, or busy",
                "unexpected result for zero-size WASM blob");
        }
    }
}
