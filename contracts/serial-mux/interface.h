/*
 * contracts/serial-mux/interface.h — Serial/UART Multiplexer Generic Device Interface
 *
 * // STATUS: IMPLEMENTED
 *
 * This is the canonical contract for the serial-mux device service in agentOS.
 * The concrete implementation lives in kernel/agentos-root-task/src/console_mux.c.
 *
 * The serial-mux service multiplexes per-PD output streams onto the debug UART
 * (and optionally onto guest UART devices) with open/close/read/write semantics.
 * Every guest OS and VMM must use this service for console I/O rather than
 * driving UART hardware directly.
 *
 * IPC transport:
 *   - Protected procedure call (PPC) via seL4 Microkit.
 *   - MR0 = opcode (SERIAL_MUX_OP_*)
 *   - MR1..MR6 = arguments (opcode-specific, see per-op comments below)
 *   - Reply: MR0 = status (SERIAL_MUX_ERR_*), MR1..MR6 = result fields
 *
 * Shared-memory ring layout (per session, SERIAL_MUX_RING_SIZE bytes):
 *   [0  .. 3 ] magic    (SERIAL_MUX_RING_MAGIC)
 *   [4  .. 7 ] pd_id
 *   [8  .. 11] head     (write offset, updated by client PD)
 *   [12 .. 15] tail     (read offset, updated by serial-mux)
 *   [16 .. N ] character ring buffer
 *
 * Capability grant:
 *   vm_manager.c grants a PPC capability to the serial-mux endpoint at
 *   guest OS creation time.  The guest discovers its session slot id via
 *   the SERIAL_MUX_OP_OPEN reply.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

/* ── Interface version ──────────────────────────────────────────────────── */
#define SERIAL_MUX_INTERFACE_VERSION    1

/* ── Ring buffer constants ──────────────────────────────────────────────── */
#define SERIAL_MUX_RING_MAGIC           0xC0DE4D55u  /* "CODEMUX" */
#define SERIAL_MUX_RING_SIZE            4096u        /* bytes per session ring */
#define SERIAL_MUX_RING_HDR_SIZE        16u
#define SERIAL_MUX_RING_BUF_SIZE        (SERIAL_MUX_RING_SIZE - SERIAL_MUX_RING_HDR_SIZE)
#define SERIAL_MUX_MAX_SESSIONS         16u

/* ── Display / multiplexer modes (used with SERIAL_MUX_OP_SET_MODE) ────── */
#define SERIAL_MUX_MODE_SINGLE          0u  /* show only the attached session */
#define SERIAL_MUX_MODE_BROADCAST       1u  /* show all sessions, tagged     */
#define SERIAL_MUX_MODE_SPLIT           2u  /* show up to 4 sessions, labelled */

/* ── Opcodes (MR0) ──────────────────────────────────────────────────────── */

/*
 * SERIAL_MUX_OP_OPEN (0x80)
 * Open a new serial session for the calling PD.
 *
 * Request:
 *   MR1 = pd_id           — caller's protection domain identifier
 *   MR2 = requested_slot  — preferred ring slot (0..MAX_SESSIONS-1),
 *                           or SERIAL_MUX_SLOT_ANY (0xFF) for auto-assign
 * Reply:
 *   MR0 = status          — SERIAL_MUX_ERR_OK or error
 *   MR1 = assigned_slot   — ring buffer slot index granted to this session
 */
#define SERIAL_MUX_OP_OPEN              0x80u
#define SERIAL_MUX_SLOT_ANY             0xFFu

/*
 * SERIAL_MUX_OP_CLOSE (0x81)
 * Close an existing serial session.
 *
 * Request:
 *   MR1 = slot            — ring buffer slot to release
 * Reply:
 *   MR0 = status
 */
#define SERIAL_MUX_OP_CLOSE             0x81u

/*
 * SERIAL_MUX_OP_READ (0x82)
 * Read characters injected into this session (e.g., keyboard input).
 *
 * Characters are placed in the shared ring before this call; this op
 * returns how many bytes are waiting and atomically advances the read pointer.
 *
 * Request:
 *   MR1 = slot            — session slot
 *   MR2 = max_bytes       — maximum bytes caller is willing to consume
 * Reply:
 *   MR0 = status
 *   MR1 = bytes_available — bytes placed in the ring by serial-mux
 *
 * Note: actual data is in the shared ring buffer; caller reads directly.
 */
#define SERIAL_MUX_OP_READ              0x82u

/*
 * SERIAL_MUX_OP_WRITE (0x83)
 * Flush the caller's output ring to the UART (or virtual console).
 *
 * The caller writes characters into its ring buffer, then calls this op
 * to request an immediate drain (without waiting for the periodic drain
 * triggered by the notification path).
 *
 * Request:
 *   MR1 = slot            — session slot
 *   MR2 = pd_id           — caller's PD id (for ring registration if needed)
 * Reply:
 *   MR0 = status
 *   MR1 = bytes_drained   — number of bytes flushed to UART this call
 */
#define SERIAL_MUX_OP_WRITE             0x83u

/*
 * SERIAL_MUX_OP_SET_MODE (0x84)
 * Change the display multiplexing mode.
 *
 * Request:
 *   MR1 = mode            — SERIAL_MUX_MODE_*
 * Reply:
 *   MR0 = status
 */
#define SERIAL_MUX_OP_SET_MODE          0x84u

/*
 * SERIAL_MUX_OP_GET_STATUS (0x85)
 * Query current multiplexer and session state.
 *
 * Request:
 *   MR1 = slot            — session slot to query (0xFFFFFFFF = global status)
 * Reply:
 *   MR0 = status
 *   MR1 = active_pd       — PD id of the currently attached session
 *                           (0xFFFFFFFF if none)
 *   MR2 = display_mode    — current SERIAL_MUX_MODE_*
 *   MR3 = session_count   — number of registered sessions
 *   MR4 = bytes_total_lo  — low 32 bits of total bytes for the queried slot
 *   MR5 = bytes_total_hi  — high 32 bits
 */
#define SERIAL_MUX_OP_GET_STATUS        0x85u

/*
 * SERIAL_MUX_OP_ATTACH (0x86)
 * Attach the active console view to a specific session (single mode).
 *
 * Request:
 *   MR1 = pd_id           — PD to attach to
 * Reply:
 *   MR0 = status
 */
#define SERIAL_MUX_OP_ATTACH            0x86u

/*
 * SERIAL_MUX_OP_DETACH (0x87)
 * Detach from the current single-session view; return to broadcast mode.
 *
 * Request: (none)
 * Reply:
 *   MR0 = status
 */
#define SERIAL_MUX_OP_DETACH            0x87u

/* ── Error / status codes (MR0 in replies) ──────────────────────────────── */
#define SERIAL_MUX_ERR_OK               0u   /* success */
#define SERIAL_MUX_ERR_NO_SLOTS         1u   /* session table full */
#define SERIAL_MUX_ERR_NOT_FOUND        2u   /* slot or pd_id not registered */
#define SERIAL_MUX_ERR_INVAL            3u   /* invalid argument (e.g., bad mode) */
#define SERIAL_MUX_ERR_PERM             4u   /* capability check failed */
#define SERIAL_MUX_ERR_OVERFLOW         5u   /* ring buffer overflowed */

/* ── Shared-memory ring header (mapped by both client and serial-mux) ───── */

typedef struct __attribute__((packed)) {
    uint32_t magic;   /* SERIAL_MUX_RING_MAGIC; 0 = slot unused */
    uint32_t pd_id;   /* owning protection domain */
    uint32_t head;    /* write offset: updated by client PD (output) or
                         serial-mux (input injection) */
    uint32_t tail;    /* read offset: updated by serial-mux (output) or
                         client PD (input consumption) */
    /* SERIAL_MUX_RING_BUF_SIZE bytes of data follow immediately */
} serial_mux_ring_hdr_t;

/* ── Request / reply structs for callers that prefer typed access ────────── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;
    uint32_t slot;
    uint32_t pd_id;
    uint32_t arg;    /* mode, max_bytes, or requested_slot depending on op */
} serial_mux_req_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* SERIAL_MUX_ERR_* */
    uint32_t field1;          /* assigned_slot / bytes_available / active_pd */
    uint32_t field2;          /* display_mode / bytes_drained / session_count */
    uint32_t field3;          /* session_count / 0 */
    uint32_t bytes_total_lo;
    uint32_t bytes_total_hi;
} serial_mux_reply_t;
