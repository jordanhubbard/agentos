/*
 * trace_recorder_test.c — contract tests for the TraceRecorder PD
 *
 * Covered opcodes:
 *   OP_TRACE_START (0x80) — begin recording; reset buffer
 *   OP_TRACE_STOP  (0x81) — stop recording; finalize
 *   OP_TRACE_QUERY (0x82) — query: event_count, bytes_used
 *   OP_TRACE_DUMP  (0x83) — serialize to JSONL in trace_out region
 *
 * Channel: CH_TRACE_CTRL (6) from the controller's perspective.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_trace_recorder_tests(microkit_channel ch)
{
    TEST_SECTION("trace_recorder");

    /* QUERY — get current event count and bytes_used. */
    ASSERT_IPC_OK(ch, OP_TRACE_QUERY, "trace_recorder: QUERY returns ok");

    /* START — begin (or restart) a recording session. */
    ASSERT_IPC_OK_OR_ERR(ch, OP_TRACE_START, AOS_ERR_BUSY,
                         "trace_recorder: START returns ok or busy");

    /* QUERY — verify event_count is accessible while recording. */
    ASSERT_IPC_OK(ch, OP_TRACE_QUERY, "trace_recorder: QUERY while recording returns ok");

    /* STOP — finalize the session. */
    ASSERT_IPC_OK_OR_ERR(ch, OP_TRACE_STOP, AOS_ERR_NOT_FOUND,
                         "trace_recorder: STOP returns ok or not-found");

    /*
     * DUMP — serialize to the trace_out shared memory region.
     * May return ok (0 events) or unimpl if trace_out not mapped.
     */
    ASSERT_IPC_OK_OR_ERR(ch, OP_TRACE_DUMP, AOS_ERR_UNIMPL,
                         "trace_recorder: DUMP returns ok or unimpl");

    /* QUERY after STOP — buffer should be accessible. */
    ASSERT_IPC_OK(ch, OP_TRACE_QUERY, "trace_recorder: QUERY after STOP returns ok");
}
