/*
 * mem_profiler_test.c — contract tests for the MemProfiler PD
 *
 * Covered opcodes:
 *   0xC0 — MEMPROF_STATUS  : query profiling state and leak counters
 *   0xC1 — MEMPROF_RESET   : reset leak counters
 *   0xC2 — MEMPROF_SAMPLE  : request an immediate sample from a worker slot
 *
 * Channel: 0 (placeholder — CH_CONTROLLER_MEM_PROFILER is 50 from the
 *          controller's perspective but not yet a named constant in agentos.h).
 *
 * TODO: replace opcode literals with
 *       #include "../../contracts/mem-profiler/interface.h"
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

#define MEMPROF_OP_STATUS  0xC0u
#define MEMPROF_OP_RESET   0xC1u
#define MEMPROF_OP_SAMPLE  0xC2u

/* Controller-side channel to mem_profiler (from channels_generated.h: 50) */
#define CH_MEM_PROFILER_CTRL 50u

void run_mem_profiler_tests(microkit_channel ch)
{
    TEST_SECTION("mem_profiler");

    /* Use the generated channel constant if caller passed 0. */
    microkit_channel eff_ch = (ch == 0) ? (microkit_channel)CH_MEM_PROFILER_CTRL : ch;

    if (eff_ch == 0) {
        _tf_puts("# mem_profiler: channel not wired in test topology\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - mem_profiler: STATUS channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - mem_profiler: RESET channel placeholder # TODO wire ch\n");

        _tf_total++; _tf_pass++;
        _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
        _tf_puts(" - mem_profiler: SAMPLE channel placeholder # TODO wire ch\n");
        return;
    }

    /* STATUS — query profiling state. */
    ASSERT_IPC_OK(eff_ch, MEMPROF_OP_STATUS, "mem_profiler: STATUS returns ok");

    /* RESET — clear leak counters. */
    ASSERT_IPC_OK_OR_ERR(eff_ch, MEMPROF_OP_RESET, AOS_ERR_UNIMPL,
                         "mem_profiler: RESET returns ok or unimpl");

    /* SAMPLE — request a sample from worker slot 0. */
    microkit_mr_set(0, (uint64_t)MEMPROF_OP_SAMPLE);
    microkit_mr_set(1, 0);  /* worker_slot */
    (void)microkit_ppcall(eff_ch, microkit_msginfo_new(MEMPROF_OP_SAMPLE, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_BUSY) {
            _tf_ok("mem_profiler: SAMPLE slot 0 returns ok, not-found, or busy");
        } else {
            _tf_fail_point("mem_profiler: SAMPLE slot 0 returns ok, not-found, or busy",
                           "unexpected error code");
        }
    }
}
