/*
 * LogDrain IPC Contract
 *
 * The LogDrain PD drains per-PD log ring buffers to the debug UART.
 * Each PD writes into a 4KB slot in the shared log_drain_rings region,
 * then notifies LogDrain to flush it.
 *
 * Channel: CH_LOG_DRAIN (see agentos.h)
 * Opcodes: OP_LOG_WRITE (0x87), OP_LOG_STATUS (0x86)
 *
 * Invariants:
 *   - OP_LOG_WRITE registers the ring slot on first use and drains it.
 *   - OP_LOG_STATUS is read-only.
 *   - Drop semantics: if the ring is full, new log data is silently dropped.
 *     LogDrain never blocks the calling PD.
 *   - The log_drain_rings_vaddr extern must be mapped before calling
 *     log_drain_write() (via the setvar_vaddr Microkit mechanism).
 *   - This contract is a Phase 0 result; MSG_CONSOLE_* opcodes are removed.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
/* CH_LOG_DRAIN is board-specific; see agentos.h */

/* ─── Request structs ────────────────────────────────────────────────────── */

struct log_drain_req_write {
    uint32_t op;                /* OP_LOG_WRITE */
    uint32_t slot;              /* per-PD ring slot index (0..MAX_LOG_RINGS-1) */
    uint32_t pd_id;             /* TRACE_PD_* identifier for this PD */
};

struct log_drain_req_status {
    uint32_t op;                /* OP_LOG_STATUS */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct log_drain_reply_write {
    uint32_t ok;
    uint32_t bytes_drained;     /* bytes flushed from ring in this call */
};

struct log_drain_reply_status {
    uint32_t ring_count;        /* number of registered rings */
    uint32_t bytes_lo;          /* total bytes drained since boot (low 32 bits) */
    uint32_t bytes_hi;
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum log_drain_error {
    LOG_DRAIN_OK              = 0,
    LOG_DRAIN_ERR_BAD_SLOT    = 1,  /* slot >= MAX_LOG_RINGS */
    LOG_DRAIN_ERR_BAD_MAGIC   = 2,  /* ring header magic mismatch */
    LOG_DRAIN_ERR_NOT_MAPPED  = 3,  /* log_drain_rings_vaddr == 0 */
};
