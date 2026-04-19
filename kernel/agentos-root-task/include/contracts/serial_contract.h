#pragma once
/* SERIAL_PD contract — version 1
 * PD: serial_pd | Source: src/serial_pd.c | Channel: CH_SERIAL_PD (61) from controller
 * Provides OS-neutral IPC serial port access. Replaces direct dbg_puts after boot.
 */
#include <stdint.h>
#include <stdbool.h>

#define SERIAL_PD_CONTRACT_VERSION 1

/* ── Channel ─────────────────────────────────────────────────────────────── */
#define CH_SERIAL_PD  61u

/* ── Opcodes (from agentos_msg_tag_t) ───────────────────────────────────── */
#define SERIAL_OP_OPEN       0x1000u
#define SERIAL_OP_CLOSE      0x1001u
#define SERIAL_OP_WRITE      0x1002u
#define SERIAL_OP_READ       0x1003u
#define SERIAL_OP_STATUS     0x1004u
#define SERIAL_OP_CONFIGURE  0x1005u

#define SERIAL_MAX_CLIENTS   8u
#define SERIAL_WRITE_MAX     256u

/* ── Request structs ─────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t op;          /* SERIAL_OP_OPEN */
} serial_req_open_t;

typedef struct __attribute__((packed)) {
    uint32_t op;          /* SERIAL_OP_CLOSE */
    uint32_t client_slot;
} serial_req_close_t;

typedef struct __attribute__((packed)) {
    uint32_t op;           /* SERIAL_OP_WRITE */
    uint32_t client_slot;
    uint32_t shmem_offset; /* offset into serial_shmem */
    uint32_t len;          /* byte count, max SERIAL_WRITE_MAX */
} serial_req_write_t;

typedef struct __attribute__((packed)) {
    uint32_t op;           /* SERIAL_OP_READ */
    uint32_t client_slot;
    uint32_t shmem_offset;
    uint32_t max_len;
} serial_req_read_t;

typedef struct __attribute__((packed)) {
    uint32_t op;           /* SERIAL_OP_STATUS */
    uint32_t client_slot;
} serial_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t op;           /* SERIAL_OP_CONFIGURE */
    uint32_t client_slot;
    uint32_t baud_rate;
    uint32_t flags;        /* parity, stop bits, flow control bitmask */
} serial_req_configure_t;

/* ── Reply structs ───────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t result;       /* 0 = ok, non-zero = error */
    uint32_t client_slot;  /* assigned slot */
} serial_reply_open_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t bytes_written;
} serial_reply_write_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t bytes_read;
} serial_reply_read_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t baud_rate;
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t error_flags;
} serial_reply_status_t;

/* ── Error codes ─────────────────────────────────────────────────────────── */
typedef enum {
    SERIAL_OK             = 0,
    SERIAL_ERR_NO_SLOT    = 1,  /* no free client slots */
    SERIAL_ERR_BAD_SLOT   = 2,  /* invalid slot */
    SERIAL_ERR_BAD_LEN    = 3,  /* len > SERIAL_WRITE_MAX */
    SERIAL_ERR_HW         = 4,  /* hardware error */
    SERIAL_ERR_NOT_IMPL   = 5,  /* operation not yet implemented */
} serial_error_t;

/* ── Invariants ──────────────────────────────────────────────────────────
 * - After MSG_TYPE_SYSTEM_READY on EventBus, all serial output must go through IPC.
 * - dbg_puts is only valid before serial_pd reports ready.
 * - SERIAL_WRITE_MAX enforced; larger writes must be split.
 */
