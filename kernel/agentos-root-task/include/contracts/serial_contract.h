/*
 * Serial Device PD IPC Contract
 *
 * The serial_pd owns the UART hardware exclusively via a seL4 device frame
 * capability.  Callers open a port, perform I/O, and close it.
 *
 * Channel: CH_SERIAL_PD (see agentos.h)
 * Opcodes: MSG_SERIAL_* (see agentos.h)
 *
 * Invariants:
 *   - MSG_SERIAL_OPEN returns a client_slot; all subsequent calls use it.
 *   - Only one client may hold the same port_id at a time.
 *   - MSG_SERIAL_WRITE data must be placed in serial_shmem before the call.
 *     Up to 256 bytes may be written per call.
 *   - MSG_SERIAL_READ returns available byte count; data placed in serial_shmem.
 *   - MSG_SERIAL_CONFIGURE is valid only on an open client_slot.
 *   - dbg_puts() is only used before serial_pd is initialized (early boot).
 *     After MSG_SERIAL_STATUS reports port as ready, all output goes via IPC.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define SERIAL_PD_CH_CONTROLLER  CH_SERIAL_PD

/* ─── Configuration ──────────────────────────────────────────────────────── */
#define SERIAL_MAX_WRITE_BYTES  256u
#define SERIAL_MAX_CLIENTS      8u

/* ─── Request structs ────────────────────────────────────────────────────── */

struct serial_req_open {
    uint32_t port_id;           /* 0 = first/only UART */
};

struct serial_req_close {
    uint32_t client_slot;
};

struct serial_req_write {
    uint32_t client_slot;
    uint32_t len;               /* bytes in serial_shmem (max SERIAL_MAX_WRITE_BYTES) */
};

struct serial_req_read {
    uint32_t client_slot;
    uint32_t max;               /* max bytes to place in serial_shmem */
};

struct serial_req_status {
    uint32_t client_slot;
};

struct serial_req_configure {
    uint32_t client_slot;
    uint32_t baud;              /* e.g. 115200 */
    uint32_t flags;             /* SERIAL_FLAG_* */
};

#define SERIAL_FLAG_PARITY_NONE  0x00u
#define SERIAL_FLAG_PARITY_EVEN  0x01u
#define SERIAL_FLAG_PARITY_ODD   0x02u
#define SERIAL_FLAG_STOP_1       0x00u
#define SERIAL_FLAG_STOP_2       0x10u
#define SERIAL_FLAG_FLOW_NONE    0x00u
#define SERIAL_FLAG_FLOW_RTS_CTS 0x20u

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct serial_reply_open {
    uint32_t ok;
    uint32_t client_slot;
};

struct serial_reply_close {
    uint32_t ok;
};

struct serial_reply_write {
    uint32_t ok;
    uint32_t written;
};

struct serial_reply_read {
    uint32_t ok;
    uint32_t count;             /* bytes placed in serial_shmem */
};

struct serial_reply_status {
    uint32_t ok;
    uint32_t baud;
    uint32_t rx_count;          /* bytes received since open */
    uint32_t tx_count;          /* bytes transmitted since open */
    uint32_t error_flags;       /* SERIAL_ERR_* bitmask */
};

#define SERIAL_ERR_OVERRUN  (1u << 0)
#define SERIAL_ERR_FRAMING  (1u << 1)
#define SERIAL_ERR_PARITY   (1u << 2)

struct serial_reply_configure {
    uint32_t ok;
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum serial_error {
    SERIAL_OK               = 0,
    SERIAL_ERR_NO_SLOTS     = 1,  /* SERIAL_MAX_CLIENTS reached */
    SERIAL_ERR_BAD_PORT     = 2,
    SERIAL_ERR_BAD_SLOT     = 3,
    SERIAL_ERR_BAD_BAUD     = 4,
    SERIAL_ERR_NOT_OPEN     = 5,
};
