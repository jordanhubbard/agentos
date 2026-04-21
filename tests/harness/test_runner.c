/*
 * test_runner.c — agentOS Phase 5 test orchestrator PD
 *
 * This is the Microkit protection domain that runs all Phase 5 contract and
 * integration tests in sequence.  It acts as the "controller" in the test
 * system topology: it holds channels to every PD under test and drives them
 * via PPCs.
 *
 * Role in the system:
 *   init()      — called once by Microkit after all PDs are initialised;
 *                 runs every test suite in order then emits the TAP summary.
 *   notified()  — stub; test runner does not handle asynchronous notifications.
 *   protected() — stub; test runner does not accept PPCs from other PDs.
 *
 * Build notes:
 *   Include paths required:
 *     -I kernel/agentos-root-task/include   (agentos.h, microkit.h)
 *     -I tests/harness                      (test_framework.h)
 *     -I tests/contracts                    (run_*_tests declarations)
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

/* ── Forward declarations for each contract test suite ───────────────────── */

void run_eventbus_tests(microkit_channel ch);
void run_init_agent_tests(microkit_channel ch);
void run_vibe_engine_tests(microkit_channel ch);
void run_agentfs_tests(microkit_channel ch);
void run_agent_pool_tests(microkit_channel ch);
void run_worker_tests(microkit_channel ch);
void run_gpu_sched_tests(microkit_channel ch);
void run_log_drain_tests(microkit_channel ch);
void run_vibe_swap_tests(microkit_channel ch);
void run_mesh_agent_tests(microkit_channel ch);
void run_cap_audit_log_tests(microkit_channel ch);
void run_quota_pd_tests(microkit_channel ch);
void run_snapshot_sched_tests(microkit_channel ch);
void run_debug_bridge_tests(microkit_channel ch);
void run_fault_handler_tests(microkit_channel ch);
void run_mem_profiler_tests(microkit_channel ch);
void run_perf_counters_tests(microkit_channel ch);
void run_net_isolator_tests(microkit_channel ch);
void run_cap_broker_tests(microkit_channel ch);
void run_watchdog_tests(microkit_channel ch);
void run_trace_recorder_tests(microkit_channel ch);
void run_oom_killer_tests(microkit_channel ch);
void run_time_partition_tests(microkit_channel ch);
void run_vm_manager_tests(microkit_channel ch);
void run_serial_pd_tests(microkit_channel ch);
void run_net_pd_tests(microkit_channel ch);
void run_block_pd_tests(microkit_channel ch);

/* ── Forward declarations for integration test suites ────────────────────── */

void run_guest_binding_tests(void);
void run_vibeos_lifecycle_tests(void);
void run_vibe_hotswap_tests(void);

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void)
{
    tf_tap_init("agentOS-phase5");

    /* ── Contract tests (one suite per PD) ──────────────────────────────── */
    run_eventbus_tests(MONITOR_CH_EVENTBUS);
    run_init_agent_tests(MONITOR_CH_INITAGENT);
    run_vibe_engine_tests((microkit_channel)CH_VIBEENGINE);
    run_agentfs_tests(0);            /* placeholder: agentfs channel TBD */
    run_agent_pool_tests(0);         /* placeholder: agent_pool channel TBD */
    run_worker_tests((microkit_channel)WORKER_POOL_BASE_CH);
    run_gpu_sched_tests(0);          /* placeholder: gpu_sched channel TBD */
    run_log_drain_tests((microkit_channel)CH_LOG_DRAIN);
    run_vibe_swap_tests((microkit_channel)SWAP_SLOT_BASE_CH);
    run_mesh_agent_tests(0);         /* placeholder: mesh_agent channel TBD */
    run_cap_audit_log_tests((microkit_channel)CH_CAP_AUDIT_CTRL);
    run_quota_pd_tests((microkit_channel)CH_QUOTA_CTRL);
    run_snapshot_sched_tests(0);     /* placeholder: snapshot_sched channel TBD */
    run_debug_bridge_tests(0);       /* placeholder: debug_bridge channel TBD */
    run_fault_handler_tests(0);      /* placeholder: fault_handler channel TBD */
    run_mem_profiler_tests(0);       /* placeholder: mem_profiler channel TBD */
    run_perf_counters_tests(0);      /* placeholder: perf_counters channel TBD */
    run_net_isolator_tests(0);       /* placeholder: net_isolator channel TBD */
    run_cap_broker_tests(0);         /* placeholder: cap_broker channel TBD */
    run_watchdog_tests((microkit_channel)CH_WATCHDOG_CTRL);
    run_trace_recorder_tests((microkit_channel)CH_TRACE_CTRL);
    run_oom_killer_tests(0);         /* placeholder: oom_killer channel TBD */
    run_time_partition_tests(0);     /* placeholder: time_partition channel TBD */
    run_vm_manager_tests((microkit_channel)CH_VM_MANAGER);
    run_serial_pd_tests((microkit_channel)CH_SERIAL_PD);
    run_net_pd_tests((microkit_channel)CH_NET_PD);
    run_block_pd_tests((microkit_channel)CH_BLOCK_PD);

    /* ── Integration tests ───────────────────────────────────────────────── */
    run_guest_binding_tests();
    run_vibeos_lifecycle_tests();
    run_vibe_hotswap_tests();

    tf_tap_finish();
}

void notified(microkit_channel ch)
{
    /* Test runner does not handle notifications; silence the unused-param warning. */
    (void)ch;
}

seL4_MessageInfo_t protected(microkit_channel ch, seL4_MessageInfo_t msginfo)
{
    /* Test runner does not serve PPCs from other PDs. */
    (void)ch;
    microkit_mr_set(0, AOS_ERR_UNIMPL);
    return microkit_msginfo_new(AOS_ERR_UNIMPL, 1);
}
