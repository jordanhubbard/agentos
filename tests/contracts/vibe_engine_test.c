/*
 * vibe_engine_test.c — contract tests for the VibeEngine PD
 *
 * Covered opcodes:
 *   OP_VIBE_STATUS           (0x43) — query overall vibe engine state
 *   OP_VIBE_HEALTH           (0x45) — probe current active slot health
 *   OP_VIBE_REPLAY           (0x46) — trigger boot-time registry replay
 *   OP_VIBE_HOTRELOAD        (0x47) — initiate zero-downtime slot update
 *   OP_VIBE_REGISTRY_STATUS  (0x48) — query registry entry count + stats
 *   OP_VIBE_REGISTRY_QUERY   (0x4B) — query registry by hash
 *
 * Channel: CH_VIBEENGINE (40) from the controller's perspective.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_vibe_engine_tests(microkit_channel ch)
{
    TEST_SECTION("vibe_engine");

    /* STATUS — engine must respond with ok. */
    ASSERT_IPC_OK(ch, OP_VIBE_STATUS, "vibe_engine: STATUS returns ok");

    /* REGISTRY_STATUS — must return ok with registry stats. */
    ASSERT_IPC_OK(ch, OP_VIBE_REGISTRY_STATUS,
                  "vibe_engine: REGISTRY_STATUS returns ok");

    /* HEALTH — probe active slot; may be unimpl if no slot is loaded yet. */
    ASSERT_IPC_OK_OR_ERR(ch, OP_VIBE_HEALTH, AOS_ERR_UNIMPL,
                         "vibe_engine: HEALTH returns ok or unimpl");

    /*
     * REPLAY — seeds registry from AgentFS; acceptable results are ok or
     * unimpl (if AgentFS is not yet connected in this test topology).
     */
    ASSERT_IPC_OK_OR_ERR(ch, OP_VIBE_REPLAY, AOS_ERR_UNIMPL,
                         "vibe_engine: REPLAY returns ok or unimpl");

    /*
     * HOTRELOAD — zero-downtime update; without a valid staging region this
     * should return an error (INVAL or UNIMPL) rather than crashing.
     */
    microkit_mr_set(0, (uint64_t)OP_VIBE_HOTRELOAD);
    microkit_mr_set(1, 0);  /* service_id = 0 (invalid) */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VIBE_HOTRELOAD, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_INVAL || rc == AOS_ERR_UNIMPL ||
            rc == (uint64_t)HOTRELOAD_FALLBACK || rc == (uint64_t)HOTRELOAD_ERR_CAPS) {
            _tf_ok("vibe_engine: HOTRELOAD with invalid svc_id returns structured error");
        } else {
            _tf_fail_point(
                "vibe_engine: HOTRELOAD with invalid svc_id returns structured error",
                "unexpected error code");
        }
    }

    /*
     * REGISTRY_QUERY — query by zero hash; expect not-found or ok.
     */
    microkit_mr_set(0, (uint64_t)OP_VIBE_REGISTRY_QUERY);
    microkit_mr_set(1, 0);  /* hash_lo = 0 */
    microkit_mr_set(2, 0);  /* hash_hi = 0 */
    (void)microkit_ppcall(ch, microkit_msginfo_new(OP_VIBE_REGISTRY_QUERY, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("vibe_engine: REGISTRY_QUERY returns ok or not-found");
        } else {
            _tf_fail_point("vibe_engine: REGISTRY_QUERY returns ok or not-found",
                           "unexpected error code");
        }
    }
}
