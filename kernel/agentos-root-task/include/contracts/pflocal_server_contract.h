/*
 * PflocalServer IPC Contract
 *
 * The PflocalServer PD implements AF_LOCAL (Unix domain) socket semantics
 * within agentOS.  It provides stream-oriented IPC between PDs and guest
 * OS processes without requiring network stack involvement.  All data
 * transfer is through pflocal_shmem; IPC opcodes manage the socket
 * state machine only.
 *
 * Channel: CH_PFLOCAL_SERVER = 26 (from controller perspective)
 * Opcodes: OP_PFLOCAL_SOCKET, OP_PFLOCAL_BIND, OP_PFLOCAL_LISTEN,
 *          OP_PFLOCAL_CONNECT, OP_PFLOCAL_ACCEPT, OP_PFLOCAL_SEND,
 *          OP_PFLOCAL_RECV, OP_PFLOCAL_CLOSE, OP_PFLOCAL_STATUS
 *
 * Shmem: pflocal_shmem (64 KB)
 *   Layout: 16 socket slots × 4 KB each
 *   Each slot holds the socket's data ring; slot_offset (from SOCKET reply)
 *   is the byte offset of the slot in pflocal_shmem.
 *   BIND and CONNECT path strings are NUL-terminated and placed at the
 *   start of the caller's slot before the IPC call.
 *
 * Invariants:
 *   - A socket must be BOUND before it may LISTEN.
 *   - CONNECT requires a listening server socket at the given path.
 *   - ACCEPT produces a new sock_id for the accepted peer; the original
 *     listening sock_id remains in SOCK_LISTENING state.
 *   - SEND / RECV use the slot ring identified by the sock_id.
 *   - CLOSE on a connected socket notifies the peer; subsequent RECV
 *     returns any buffered data then PFLOCAL_ERR_NOT_FOUND.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define CH_PFLOCAL_SERVER   26   /* controller → pflocal_server */

/* ─── Opcodes ─────────────────────────────────────────────────────────────── */
#define OP_PFLOCAL_SOCKET    0x01
#define OP_PFLOCAL_BIND      0x02
#define OP_PFLOCAL_LISTEN    0x03
#define OP_PFLOCAL_CONNECT   0x04
#define OP_PFLOCAL_ACCEPT    0x05
#define OP_PFLOCAL_SEND      0x06
#define OP_PFLOCAL_RECV      0x07
#define OP_PFLOCAL_CLOSE     0x08
#define OP_PFLOCAL_STATUS    0x09

/* ─── Socket states ──────────────────────────────────────────────────────── */
#define SOCK_FREE        0       /* slot unused */
#define SOCK_CREATED     1       /* allocated, not yet bound */
#define SOCK_BOUND       2       /* path registered, not listening */
#define SOCK_LISTENING   3       /* accept() queue active */
#define SOCK_CONNECTED   4       /* connected as client */
#define SOCK_ACCEPTED    5       /* server-side peer of an accepted connection */

/* ─── Shmem constants ────────────────────────────────────────────────────── */
#define PFLOCAL_SOCK_SLOTS     16u
#define PFLOCAL_SLOT_BYTES     (4u * 1024u)
#define PFLOCAL_SHMEM_TOTAL    (PFLOCAL_SOCK_SLOTS * PFLOCAL_SLOT_BYTES)

/* ─── Request structs ────────────────────────────────────────────────────── */

struct pflocal_server_req_socket {
    uint32_t op;                 /* OP_PFLOCAL_SOCKET */
};

struct pflocal_server_req_bind {
    uint32_t op;                 /* OP_PFLOCAL_BIND */
    uint32_t sock_id;
    /* NUL-terminated path at start of caller's slot in pflocal_shmem */
};

struct pflocal_server_req_listen {
    uint32_t op;                 /* OP_PFLOCAL_LISTEN */
    uint32_t sock_id;
};

struct pflocal_server_req_connect {
    uint32_t op;                 /* OP_PFLOCAL_CONNECT */
    uint32_t sock_id;
    /* NUL-terminated server path at start of caller's slot in pflocal_shmem */
};

struct pflocal_server_req_accept {
    uint32_t op;                 /* OP_PFLOCAL_ACCEPT */
    uint32_t sock_id;            /* listening socket */
};

struct pflocal_server_req_send {
    uint32_t op;                 /* OP_PFLOCAL_SEND */
    uint32_t sock_id;
    uint32_t offset;             /* byte offset in pflocal_shmem of data */
    uint32_t len;                /* bytes to send */
};

struct pflocal_server_req_recv {
    uint32_t op;                 /* OP_PFLOCAL_RECV */
    uint32_t sock_id;
    uint32_t offset;             /* byte offset in pflocal_shmem for output */
    uint32_t max;                /* maximum bytes to receive */
};

struct pflocal_server_req_close {
    uint32_t op;                 /* OP_PFLOCAL_CLOSE */
    uint32_t sock_id;
};

struct pflocal_server_req_status {
    uint32_t op;                 /* OP_PFLOCAL_STATUS */
    uint32_t sock_id;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct pflocal_server_reply_socket {
    uint32_t ok;                 /* PFLOCAL_OK or error code */
    uint32_t sock_id;            /* allocated socket index; ~0u on error */
    uint32_t slot_offset;        /* byte offset of this socket's slot in shmem */
};

struct pflocal_server_reply_bind {
    uint32_t ok;                 /* PFLOCAL_OK or error code */
};

struct pflocal_server_reply_listen {
    uint32_t ok;                 /* PFLOCAL_OK or error code */
};

struct pflocal_server_reply_connect {
    uint32_t ok;                 /* PFLOCAL_OK or error code */
};

struct pflocal_server_reply_accept {
    uint32_t ok;                 /* PFLOCAL_OK or error code */
    uint32_t new_sock_id;        /* new socket for the accepted peer */
    uint32_t peer_slot;          /* slot offset for the peer's data ring */
};

struct pflocal_server_reply_send {
    uint32_t ok;                 /* PFLOCAL_OK or error code */
    uint32_t sent;               /* bytes enqueued */
};

struct pflocal_server_reply_recv {
    uint32_t ok;                 /* PFLOCAL_OK or error code */
    uint32_t recv;               /* bytes placed in shmem */
};

struct pflocal_server_reply_close {
    uint32_t ok;                 /* PFLOCAL_OK or error code */
};

struct pflocal_server_reply_status {
    uint32_t ok;                 /* PFLOCAL_OK or error code */
    uint32_t state;              /* SOCK_* state constant */
    uint32_t peer_sock_id;       /* connected peer; ~0u if not connected */
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum pflocal_server_error {
    PFLOCAL_OK                  = 0,
    PFLOCAL_ERR_INVAL           = 1,  /* bad argument */
    PFLOCAL_ERR_NO_SOCKS        = 2,  /* all socket slots occupied */
    PFLOCAL_ERR_NOT_FOUND       = 3,  /* sock_id or path not found */
    PFLOCAL_ERR_NOT_BOUND       = 4,  /* operation requires SOCK_BOUND state */
    PFLOCAL_ERR_NOT_LISTENING   = 5,  /* ACCEPT on non-listening socket */
};
