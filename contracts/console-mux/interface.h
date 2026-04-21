/*
 * agentOS ConsoleMux IPC Contract — interface.h
 *
 * Formal seL4 IPC API contract for the ConsoleMux protection domain.
 * ConsoleMux is the serial console multiplexer.  It arbitrates access
 * to the physical UART among all PDs and provides a virtual console
 * channel per client PD.
 *
 * Design:
 *   - Shared memory ring buffer (console_ring, 16KB) for zero-copy output
 *   - Multiple clients write to their own slot in the ring
 *   - ConsoleMux serializes ring entries to UART in priority order
 *   - Each client has a 4-byte "console channel" identifier (for multiplexing)
 *   - Supports flush, query pending bytes, and cursor control
 *
 * The console_ring MR (16KB, see topology.yaml) is mapped into the
 * console_mux PD at vaddr 0x6000000 (console_ring_vaddr).
 *
 * IPC mechanism: seL4_Call / seL4_Reply (passive PD, priority 160).
 * MR0 carries the opcode on request; MR0 carries the status on reply.
 *
 * NOTE: This PD exists in topology.yaml.  The IPC protocol defined here
 * is the intended production contract; the current implementation uses
 * seL4 debug_putchar for serial output without IPC mediation (the full
 * ring-based protocol is planned for the next milestone).
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Version ─────────────────────────────────────────────────────────────── */

#define CONSOLE_MUX_INTERFACE_VERSION  1

/* ── Limits ──────────────────────────────────────────────────────────────── */

#define CONSOLE_MUX_MAX_CLIENTS     8
#define CONSOLE_MUX_RING_SIZE       0x4000u  /* 16KB console_ring MR */
#define CONSOLE_MUX_WRITE_MAX       4096     /* max bytes per WRITE call */
#define CONSOLE_MUX_READ_MAX        4096     /* max bytes per READ call */
#define CONSOLE_MUX_CHANNEL_ID_MAX  4        /* 4-char channel prefix */

/* ── Client flags ────────────────────────────────────────────────────────── */

#define CONSOLE_MUX_FLAG_COLOR      (1u << 0)  /* ANSI color sequences enabled */
#define CONSOLE_MUX_FLAG_PREFIX     (1u << 1)  /* prepend channel_id to each line */
#define CONSOLE_MUX_FLAG_TIMESTAMP  (1u << 2)  /* prepend timestamp to each line */

/* ── Opcodes ─────────────────────────────────────────────────────────────── */

#define CONSOLE_MUX_OP_OPEN         0x800u  /* register a console channel */
#define CONSOLE_MUX_OP_CLOSE        0x801u  /* deregister a console channel */
#define CONSOLE_MUX_OP_WRITE        0x802u  /* write bytes to console */
#define CONSOLE_MUX_OP_READ         0x803u  /* read pending input bytes */
#define CONSOLE_MUX_OP_FLUSH        0x804u  /* wait for output ring to drain */
#define CONSOLE_MUX_OP_STATUS       0x805u  /* ring buffer status */
#define CONSOLE_MUX_OP_SET_FLAGS    0x806u  /* update channel flags */
#define CONSOLE_MUX_OP_HEALTH       0x807u  /* liveness probe */

/* ── Error codes ─────────────────────────────────────────────────────────── */

#define CONSOLE_MUX_ERR_OK          0u
#define CONSOLE_MUX_ERR_INVALID_ARG 1u   /* null data, zero len, etc. */
#define CONSOLE_MUX_ERR_NOT_FOUND   2u   /* client_handle not registered */
#define CONSOLE_MUX_ERR_FULL        3u   /* ring buffer full; try again */
#define CONSOLE_MUX_ERR_MAX_CLIENTS 4u   /* already at CONSOLE_MUX_MAX_CLIENTS */
#define CONSOLE_MUX_ERR_TOO_BIG     5u   /* write > CONSOLE_MUX_WRITE_MAX */
#define CONSOLE_MUX_ERR_INTERNAL    99u

/* ── Request / reply structs ─────────────────────────────────────────────── */

/*
 * CONSOLE_MUX_OP_OPEN
 *
 * Register a console channel for the calling PD.
 * channel_id is a 4-character human-readable prefix (e.g. "ctrl", "wrkr").
 * Returns a client_handle used in subsequent calls.
 *
 * Request:  opcode, caller_pd, flags, channel_id (4 bytes + NUL)
 * Reply:    status, client_handle
 */
typedef struct console_mux_open_req {
    uint32_t opcode;                        /* CONSOLE_MUX_OP_OPEN */
    uint32_t caller_pd;
    uint32_t flags;                         /* CONSOLE_MUX_FLAG_* */
    char     channel_id[CONSOLE_MUX_CHANNEL_ID_MAX + 1]; /* NUL-terminated */
    uint8_t  _pad[3];
} __attribute__((packed)) console_mux_open_req_t;

typedef struct console_mux_open_rep {
    uint32_t status;                        /* CONSOLE_MUX_ERR_* */
    uint32_t client_handle;                 /* opaque handle for this client */
} __attribute__((packed)) console_mux_open_rep_t;

/*
 * CONSOLE_MUX_OP_CLOSE
 *
 * Deregister a console channel.  Pending buffered output is flushed first.
 *
 * Request:  opcode, client_handle
 * Reply:    status
 */
typedef struct console_mux_close_req {
    uint32_t opcode;                        /* CONSOLE_MUX_OP_CLOSE */
    uint32_t client_handle;
} __attribute__((packed)) console_mux_close_req_t;

typedef struct console_mux_close_rep {
    uint32_t status;
} __attribute__((packed)) console_mux_close_rep_t;

/*
 * CONSOLE_MUX_OP_WRITE
 *
 * Write bytes to the console.  Data is placed by the caller into the
 * console_ring MR at data_ring_offset before calling.  data_len must not
 * exceed CONSOLE_MUX_WRITE_MAX (4096).
 *
 * The mux serializes writes from all clients in priority order.
 * Returns CONSOLE_MUX_ERR_FULL if the ring cannot accept the data; the
 * caller should retry (implement backpressure).
 *
 * Request:  opcode, client_handle, data_ring_offset, data_len
 * Reply:    status, bytes_accepted
 */
typedef struct console_mux_write_req {
    uint32_t opcode;                        /* CONSOLE_MUX_OP_WRITE */
    uint32_t client_handle;
    uint32_t data_ring_offset;              /* offset into console_ring MR */
    uint32_t data_len;
} __attribute__((packed)) console_mux_write_req_t;

typedef struct console_mux_write_rep {
    uint32_t status;
    uint32_t bytes_accepted;
} __attribute__((packed)) console_mux_write_rep_t;

/*
 * CONSOLE_MUX_OP_READ
 *
 * Read pending console input (keyboard / serial RX) for this client.
 * Data is written into the console_ring MR at buf_ring_offset.
 * Returns bytes_read = 0 if no input is pending.
 *
 * Request:  opcode, client_handle, buf_ring_offset, buf_len
 * Reply:    status, bytes_read
 */
typedef struct console_mux_read_req {
    uint32_t opcode;                        /* CONSOLE_MUX_OP_READ */
    uint32_t client_handle;
    uint32_t buf_ring_offset;
    uint32_t buf_len;
} __attribute__((packed)) console_mux_read_req_t;

typedef struct console_mux_read_rep {
    uint32_t status;
    uint32_t bytes_read;
} __attribute__((packed)) console_mux_read_rep_t;

/*
 * CONSOLE_MUX_OP_FLUSH
 *
 * Block until all pending output for this client has been transmitted.
 *
 * Request:  opcode, client_handle
 * Reply:    status
 */
typedef struct console_mux_flush_req {
    uint32_t opcode;                        /* CONSOLE_MUX_OP_FLUSH */
    uint32_t client_handle;
} __attribute__((packed)) console_mux_flush_req_t;

typedef struct console_mux_flush_rep {
    uint32_t status;
} __attribute__((packed)) console_mux_flush_rep_t;

/*
 * CONSOLE_MUX_OP_STATUS
 *
 * Return ring buffer occupancy statistics.
 *
 * Request:  opcode, client_handle (0 = global)
 * Reply:    status, ring_bytes_used, ring_bytes_free, pending_tx_bytes,
 *           active_clients
 */
typedef struct console_mux_status_req {
    uint32_t opcode;                        /* CONSOLE_MUX_OP_STATUS */
    uint32_t client_handle;                 /* 0 = global stats */
} __attribute__((packed)) console_mux_status_req_t;

typedef struct console_mux_status_rep {
    uint32_t status;
    uint32_t ring_bytes_used;
    uint32_t ring_bytes_free;
    uint32_t pending_tx_bytes;
    uint32_t active_clients;
} __attribute__((packed)) console_mux_status_rep_t;

/*
 * CONSOLE_MUX_OP_SET_FLAGS
 *
 * Update display flags for a registered channel.
 *
 * Request:  opcode, client_handle, flags
 * Reply:    status
 */
typedef struct console_mux_set_flags_req {
    uint32_t opcode;                        /* CONSOLE_MUX_OP_SET_FLAGS */
    uint32_t client_handle;
    uint32_t flags;                         /* CONSOLE_MUX_FLAG_* */
} __attribute__((packed)) console_mux_set_flags_req_t;

typedef struct console_mux_set_flags_rep {
    uint32_t status;
} __attribute__((packed)) console_mux_set_flags_rep_t;

/*
 * CONSOLE_MUX_OP_HEALTH
 *
 * Liveness probe.
 *
 * Request:  opcode
 * Reply:    status, active_clients, version
 */
typedef struct console_mux_health_req {
    uint32_t opcode;                        /* CONSOLE_MUX_OP_HEALTH */
} __attribute__((packed)) console_mux_health_req_t;

typedef struct console_mux_health_rep {
    uint32_t status;
    uint32_t active_clients;
    uint32_t version;                       /* CONSOLE_MUX_INTERFACE_VERSION */
} __attribute__((packed)) console_mux_health_rep_t;
