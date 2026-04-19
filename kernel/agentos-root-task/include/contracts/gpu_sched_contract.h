/*
 * GPUSched IPC Contract
 *
 * The GPU Scheduler PD arbitrates access to GPU compute resources.
 * Agents submit tasks by hash; the scheduler queues them and dispatches
 * to available GPU slots.  Completion and failure are reported via EventBus.
 *
 * Channel: varies (from agent perspective; controller routes to gpu_sched)
 * Opcodes: MSG_GPU_* (see agentos.h)
 *
 * Invariants:
 *   - MSG_GPU_SUBMIT is non-blocking: the task is enqueued and a ticket_id
 *     is returned immediately.
 *   - MSG_GPU_COMPLETE and MSG_GPU_FAILED are EventBus events, not replies.
 *   - MSG_GPU_CANCEL removes a pending ticket; if dispatch has already begun,
 *     the cancel is a no-op and CANCEL_ERR_RUNNING is returned.
 *   - ticket_id 0 is reserved; a valid ticket always has ticket_id > 0.
 *   - Priority 0 = lowest, 255 = highest (mapped to MCS budget).
 */

#pragma once
#include "../agentos.h"

/* ─── Request structs ────────────────────────────────────────────────────── */

struct gpu_sched_req_submit {
    uint64_t hash_lo;           /* task WASM hash low */
    uint64_t hash_hi;           /* task WASM hash high */
    uint8_t  priority;          /* 0-255 */
    uint8_t  flags;             /* GPU_SUBMIT_FLAG_* */
    uint16_t _pad;
};

#define GPU_SUBMIT_FLAG_URGENT  (1u << 0)  /* jump queue head */
#define GPU_SUBMIT_FLAG_RETRY   (1u << 1)  /* auto-retry on transient failure */

struct gpu_sched_req_status {
    /* no fields */
};

struct gpu_sched_req_cancel {
    uint32_t ticket_id;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct gpu_sched_reply_submit {
    uint32_t ok;
    uint32_t ticket_id;         /* 0 = rejected (queue full) */
};

struct gpu_sched_reply_status {
    uint32_t queue_depth;
    uint32_t busy_slots;
    uint32_t idle_slots;
    uint32_t total_submitted;   /* since boot */
    uint32_t total_completed;
    uint32_t total_failed;
};

struct gpu_sched_reply_cancel {
    uint32_t ok;
    uint32_t result;            /* GPU_CANCEL_OK / GPU_CANCEL_ERR_* */
};

#define GPU_CANCEL_OK           0
#define GPU_CANCEL_ERR_NOTFOUND 1
#define GPU_CANCEL_ERR_RUNNING  2  /* already dispatched; cancel is a no-op */

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum gpu_sched_error {
    GPU_SCHED_OK              = 0,
    GPU_SCHED_ERR_QUEUE_FULL  = 1,
    GPU_SCHED_ERR_BAD_TICKET  = 2,
    GPU_SCHED_ERR_NO_GPU      = 3,  /* no GPU hardware present */
};
