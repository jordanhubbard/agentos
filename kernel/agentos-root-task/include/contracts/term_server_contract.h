/*
 * TermServer IPC Contract
 *
 * The TermServer PD allocates and manages pseudo-terminal (PTY) pairs for
 * guest OS serial consoles and interactive agent sessions.  Each PTY has a
 * master side (driver) and a slave side (application) mapped into term_shmem.
 * Data transfer between sides is done via lock-free ring buffers in shmem;
 * the IPC opcodes control lifecycle and geometry, not data movement.
 *
 * Channel: CH_TERM_SERVER = 43 (from controller perspective)
 * Opcodes: OP_TERM_OPENPTY, OP_TERM_RESIZE, OP_TERM_WRITE,
 *          OP_TERM_READ, OP_TERM_CLOSEPTY, OP_TERM_STATUS
 *
 * Shmem: term_shmem (16 KB)
 *   Layout: 4 PTY slots × 2 directions × 2 KB each
 *   Each 2 KB ring:
 *     [0]  uint32_t head    — producer write index
 *     [4]  uint32_t tail    — consumer read index
 *     [8]  uint32_t cap     — ring capacity in bytes (2032)
 *     [12] uint32_t magic   — TERM_RING_MAGIC sanity word
 *     [16..2047] uint8_t data[2032]
 *
 * Invariants:
 *   - OP_TERM_WRITE and OP_TERM_READ pass data through term_shmem rings,
 *     identified by shmem_offset returned at OPENPTY time.
 *   - RESIZE does not flush buffered data; the application must handle
 *     SIGWINCH semantics itself.
 *   - CLOSEPTY invalidates both ring buffers; any outstanding READ returns
 *     TERM_ERR_NOT_FOUND for the closed pty_id.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define CH_TERM_SERVER   43      /* controller → term_server */

/* ─── Opcodes ─────────────────────────────────────────────────────────────── */
#define OP_TERM_OPENPTY   0x01
#define OP_TERM_RESIZE    0x02
#define OP_TERM_WRITE     0x03
#define OP_TERM_READ      0x04
#define OP_TERM_CLOSEPTY  0x05
#define OP_TERM_STATUS    0x06

/* ─── Shmem constants ────────────────────────────────────────────────────── */
#define TERM_RING_MAGIC        0x52494E47u   /* "RING" */
#define TERM_RING_DATA_BYTES   2032u         /* usable bytes per ring buffer */
#define TERM_PTY_SLOTS         4u
#define TERM_SHMEM_TOTAL       (16u * 1024u)

/* ─── Request structs ────────────────────────────────────────────────────── */

struct term_server_req_openpty {
    uint32_t op;                 /* OP_TERM_OPENPTY */
};

struct term_server_req_resize {
    uint32_t op;                 /* OP_TERM_RESIZE */
    uint32_t pty_id;             /* pty_id from OPENPTY */
    uint32_t rows;
    uint32_t cols;
};

struct term_server_req_write {
    uint32_t op;                 /* OP_TERM_WRITE */
    uint32_t pty_id;
    uint32_t shmem_offset;       /* byte offset into term_shmem for source data */
    uint32_t len;                /* byte count to write */
};

struct term_server_req_read {
    uint32_t op;                 /* OP_TERM_READ */
    uint32_t pty_id;
    uint32_t shmem_offset;       /* byte offset into term_shmem for dest data */
    uint32_t max;                /* maximum bytes to read */
};

struct term_server_req_closepty {
    uint32_t op;                 /* OP_TERM_CLOSEPTY */
    uint32_t pty_id;
};

struct term_server_req_status {
    uint32_t op;                 /* OP_TERM_STATUS */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct term_server_reply_openpty {
    uint32_t ok;                 /* TERM_OK or error code */
    uint32_t pty_id;             /* allocated PTY index (0..3); ~0u on error */
    uint32_t master_offset;      /* byte offset of master ring in term_shmem */
    uint32_t slave_offset;       /* byte offset of slave ring in term_shmem */
};

struct term_server_reply_resize {
    uint32_t ok;                 /* TERM_OK or error code */
};

struct term_server_reply_write {
    uint32_t ok;                 /* TERM_OK or error code */
};

struct term_server_reply_read {
    uint32_t ok;                 /* TERM_OK or error code */
    uint32_t len;                /* bytes placed into shmem; 0 if ring empty */
};

struct term_server_reply_closepty {
    uint32_t ok;                 /* TERM_OK or error code */
};

struct term_server_reply_status {
    uint32_t ok;                 /* TERM_OK or error code */
    uint32_t active_ptys;        /* number of open PTY slots */
};

/* ─── Shmem layout: ring buffer header ───────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t head;               /* producer index (wraps at cap) */
    uint32_t tail;               /* consumer index (wraps at cap) */
    uint32_t cap;                /* ring capacity in bytes */
    uint32_t magic;              /* TERM_RING_MAGIC */
    uint8_t  data[TERM_RING_DATA_BYTES];
} term_ring_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum term_server_error {
    TERM_OK              = 0,
    TERM_ERR_INVAL       = 1,    /* bad argument */
    TERM_ERR_NO_PTYS     = 2,    /* all PTY slots occupied */
    TERM_ERR_NOT_FOUND   = 3,    /* pty_id not open */
};
