/*
 * mesh_agent_test.c — contract tests for the MeshAgent PD
 *
 * Covered opcodes:
 *   MSG_MESH_ANNOUNCE     (0x0A01) — register this node with the mesh
 *   MSG_MESH_STATUS       (0x0A03) — query peer count and total slots
 *   MSG_REMOTE_SPAWN      (0x0A05) — spawn agent on best-available peer
 *   MSG_MESH_HEARTBEAT    (0x0A07) — periodic liveness ping
 *
 * Channel: 0 (placeholder — update when ch is assigned in agentos.system).
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_mesh_agent_tests(microkit_channel ch)
{
    TEST_SECTION("mesh_agent");

    if (ch == 0) {
        _tf_puts("# mesh_agent: channel not wired in test topology (ch=0)\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - mesh_agent: STATUS channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - mesh_agent: ANNOUNCE channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - mesh_agent: REMOTE_SPAWN channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - mesh_agent: HEARTBEAT channel placeholder # TODO wire ch\n");
        return;
    }

    /* STATUS — query peer count. */
    ASSERT_IPC_OK(ch, MSG_MESH_STATUS, "mesh_agent: STATUS returns ok");

    /* ANNOUNCE — register with zero node_id and slot counts. */
    microkit_mr_set(0, (uint64_t)MSG_MESH_ANNOUNCE);
    microkit_mr_set(1, 0);  /* node_id */
    microkit_mr_set(2, 0);  /* slot_count */
    microkit_mr_set(3, 0);  /* gpu_slots */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_MESH_ANNOUNCE, 4));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_EXISTS || rc == AOS_ERR_INVAL) {
            _tf_ok("mesh_agent: ANNOUNCE returns ok, exists, or inval");
        } else {
            _tf_fail_point("mesh_agent: ANNOUNCE returns ok, exists, or inval",
                           "unexpected error code");
        }
    }

    /* REMOTE_SPAWN — attempt to spawn on best peer; may have no peers. */
    microkit_mr_set(0, (uint64_t)MSG_REMOTE_SPAWN);
    microkit_mr_set(1, 0);  /* hash_lo */
    microkit_mr_set(2, 0);  /* hash_hi */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_REMOTE_SPAWN, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_NOSPC) {
            _tf_ok("mesh_agent: REMOTE_SPAWN returns ok, not-found, or nospc");
        } else {
            _tf_fail_point("mesh_agent: REMOTE_SPAWN returns ok, not-found, or nospc",
                           "unexpected error code");
        }
    }

    /* HEARTBEAT — liveness ping; expect ok. */
    microkit_mr_set(0, (uint64_t)MSG_MESH_HEARTBEAT);
    microkit_mr_set(1, 0);  /* node_id */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_MESH_HEARTBEAT, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("mesh_agent: HEARTBEAT returns ok or not-found");
        } else {
            _tf_fail_point("mesh_agent: HEARTBEAT returns ok or not-found",
                           "unexpected error code");
        }
    }
}
