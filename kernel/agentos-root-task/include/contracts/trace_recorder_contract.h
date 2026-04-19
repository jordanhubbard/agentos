/*
 * TraceRecorder IPC Contract
 *
 * The TraceRecorder PD captures inter-PD IPC events in a binary ring buffer.
 * The controller notifies TraceRecorder on each dispatch; it records the
 * source, destination, and message label.
 *
 * Channel: CH_TRACE_CTRL (PPCs), CH_TRACE_NOTIFY (controller notifications)
 * Opcodes: OP_TRACE_START, OP_TRACE_STOP, OP_TRACE_QUERY, OP_TRACE_DUMP
 *
 * Invariants:
 *   - OP_TRACE_START resets the ring and begins recording.
 *   - OP_TRACE_STOP finalizes the trace; no new events are added.
 *   - OP_TRACE_QUERY returns event_count and bytes_used (non-destructive).
 *   - OP_TRACE_DUMP serializes the ring to JSONL in the trace_out shared region.
 *   - Events are packed into trace_out in order; the JSONL format is:
 *     {"src":<pd>,"dst":<pd>,"label":<hex>,"tick":<ticks>}
 *   - CH_TRACE_NOTIFY is a fast-path notify (MR0 encodes src/dst/label).
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define TRACE_RECORDER_CH_CTRL    CH_TRACE_CTRL
#define TRACE_RECORDER_CH_NOTIFY  CH_TRACE_NOTIFY

/* ─── Request structs ────────────────────────────────────────────────────── */

struct trace_req_start {
    uint32_t op;                /* OP_TRACE_START */
    uint32_t flags;             /* TRACE_FLAG_* */
};

#define TRACE_FLAG_WRAP     (1u << 0)  /* overwrite old events when ring full */
#define TRACE_FLAG_JSONL    (1u << 1)  /* dump in JSONL format (always default) */

struct trace_req_stop {
    uint32_t op;                /* OP_TRACE_STOP */
};

struct trace_req_query {
    uint32_t op;                /* OP_TRACE_QUERY */
};

struct trace_req_dump {
    uint32_t op;                /* OP_TRACE_DUMP */
    uint32_t max_events;        /* max events to serialize (0 = all) */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct trace_reply_start {
    uint32_t ok;
};

struct trace_reply_stop {
    uint32_t ok;
    uint32_t event_count;
};

struct trace_reply_query {
    uint32_t ok;
    uint32_t event_count;
    uint32_t bytes_used;
};

struct trace_reply_dump {
    uint32_t ok;
    uint32_t events_written;    /* events serialized to trace_out region */
    uint32_t bytes_written;
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum trace_recorder_error {
    TRACE_OK                  = 0,
    TRACE_ERR_NOT_STARTED     = 1,
    TRACE_ERR_ALREADY_STARTED = 2,
    TRACE_ERR_RING_FULL       = 3,  /* returned if TRACE_FLAG_WRAP not set */
    TRACE_ERR_OUTPUT_FULL     = 4,  /* trace_out region insufficient for dump */
};
