#pragma once
/* TRACE_RECORDER contract — version 1
 * PD: trace_recorder | Source: src/trace_recorder.c
 * Channel: CH_TRACE_CTRL=6 (controller PPCs in), CH_TRACE_NOTIFY=7 (controller notifies on dispatch)
 */
#include <stdint.h>
#include <stdbool.h>

#define TRACE_RECORDER_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
#define CH_TRACE_CTRL               6   /* controller -> trace_recorder (PPC START/STOP/QUERY/DUMP) */
#define CH_TRACE_NOTIFY             7   /* controller notifies trace_recorder on each dispatch */

/* ── Opcodes ── */
#define TRACE_RECORDER_OP_START     0x80u  /* begin recording; reset buffer */
#define TRACE_RECORDER_OP_STOP      0x81u  /* stop recording; finalize buffer */
#define TRACE_RECORDER_OP_QUERY     0x82u  /* query: event_count, bytes_used */
#define TRACE_RECORDER_OP_DUMP      0x83u  /* serialize to JSONL in trace_out region */
#define TRACE_RECORDER_OP_FILTER    0x84u  /* set event type filter mask */
#define TRACE_RECORDER_OP_CLEAR     0x85u  /* clear trace buffer without stopping */

/* ── Trace notify payload encoding (MR0 from controller on CH_TRACE_NOTIFY) ── */
/* MR0 = (src_pd << 24) | (dst_pd << 16) | label[15:0] */
#define TRACE_MR0_ENCODE(src, dst, label) \
    (((uint32_t)(src) << 24) | ((uint32_t)(dst) << 16) | ((uint32_t)(label) & 0xFFFFu))

/* ── Trace PD IDs (cross-ref: agentos.h TRACE_PD_*) ── */
#define TRACE_PD_CONTROLLER         0u
#define TRACE_PD_EVENT_BUS          1u
#define TRACE_PD_INIT_AGENT         2u
#define TRACE_PD_WORKER_BASE        3u   /* worker 0-7 = 3-10 */
#define TRACE_PD_AGENTFS           11u
#define TRACE_PD_VIBE_ENGINE       12u
#define TRACE_PD_SWAP_SLOT_BASE    13u   /* swap slots 0-3 = 13-16 */
#define TRACE_PD_GPU_SCHED         17u
#define TRACE_PD_MESH_AGENT        18u
#define TRACE_PD_CAP_AUDIT         19u
#define TRACE_PD_FAULT_HDL         20u
#define TRACE_PD_DEBUG_BRIDGE      21u
#define TRACE_PD_QUOTA_PD          22u
#define TRACE_PD_MEM_PROFILER      23u
#define TRACE_PD_WATCHDOG_PD       24u
#define TRACE_PD_TRACE_REC         25u

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* TRACE_RECORDER_OP_START */
    uint32_t filter_mask;     /* TRACE_FILTER_* event types to record (0 = all) */
    uint32_t flags;           /* TRACE_START_FLAG_* */
} trace_recorder_req_start_t;

#define TRACE_START_FLAG_CIRCULAR   (1u << 0)  /* overwrite oldest on full (vs. stop) */
#define TRACE_START_FLAG_TIMESTAMP  (1u << 1)  /* include nanosecond timestamps */

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} trace_recorder_reply_start_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* TRACE_RECORDER_OP_STOP */
} trace_recorder_req_stop_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t event_count;     /* events captured before stop */
    uint32_t bytes_used;      /* buffer bytes used */
} trace_recorder_reply_stop_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* TRACE_RECORDER_OP_QUERY */
} trace_recorder_req_query_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t event_count;     /* events in buffer */
    uint32_t bytes_used;      /* bytes consumed in trace buffer */
    uint32_t dropped_count;   /* events dropped due to full buffer */
    uint32_t recording;       /* 1 if actively recording */
} trace_recorder_reply_query_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* TRACE_RECORDER_OP_DUMP */
    uint32_t shmem_offset;    /* byte offset in trace_out shared region */
    uint32_t max_bytes;       /* max bytes to write */
} trace_recorder_req_dump_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t bytes_written;   /* JSONL bytes written to shmem */
    uint32_t events_written;
} trace_recorder_reply_dump_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* TRACE_RECORDER_OP_FILTER */
    uint32_t filter_mask;     /* new filter mask to apply */
} trace_recorder_req_filter_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} trace_recorder_reply_filter_t;

/* Raw trace event stored in buffer */
typedef struct __attribute__((packed)) {
    uint64_t timestamp_ns;    /* nanosecond timestamp (0 if not enabled) */
    uint32_t src_pd;          /* TRACE_PD_* source */
    uint32_t dst_pd;          /* TRACE_PD_* destination */
    uint32_t label;           /* IPC message label */
    uint32_t _pad;
} trace_recorder_event_t;

/* ── Error codes ── */
typedef enum {
    TRACE_RECORDER_OK          = 0,
    TRACE_RECORDER_ERR_RUNNING = 1,  /* START called while already recording */
    TRACE_RECORDER_ERR_STOPPED = 2,  /* STOP called while not recording */
    TRACE_RECORDER_ERR_NO_SHMEM = 3, /* DUMP called but trace_out region not mapped */
    TRACE_RECORDER_ERR_FULL    = 4,  /* buffer full and CIRCULAR not set */
} trace_recorder_error_t;

/* ── Invariants ──
 * - Exactly one recording session is active at a time; START while recording is an error.
 * - CIRCULAR mode silently overwrites the oldest event when the buffer is full.
 * - DUMP is valid whether recording is active or stopped.
 * - JSONL output format: one JSON object per line, fields: ts_ns, src, dst, label.
 * - filter_mask=0 means record all events; non-zero masks out unwanted event classes.
 * - CH_TRACE_NOTIFY is a seL4 notify (not PPC); the MR0 encoding must match TRACE_MR0_ENCODE.
 */
