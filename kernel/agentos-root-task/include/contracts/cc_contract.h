/*
 * Command-and-Control (CC) PD IPC Contract
 *
 * The CC PD provides a machine-readable external command interface for
 * agentOS.  External agents (via mesh or agentctl) connect to CC sessions
 * and submit structured commands.  This is the "external consumer" API
 * referenced in PLAN.md Phase 5.
 *
 * Channel: CH_CC_PD (see agentos.h)
 * Opcodes: MSG_CC_* (see agentos.h)
 *
 * The CC PD is NOT a UI.  It emits structured responses (binary or
 * JSON-encoded) that external tools parse.  No interactive sessions,
 * no terminal semantics.
 *
 * Invariants:
 *   - MSG_CC_CONNECT assigns a session_id; subsequent calls use it.
 *   - MSG_CC_SEND places a command (binary struct or JSON) in cc_shmem.
 *   - MSG_CC_RECV retrieves the response (placed in cc_shmem by CC PD).
 *   - Sessions expire after CC_SESSION_TIMEOUT_TICKS of inactivity.
 *   - Multiple concurrent sessions are supported (CC_MAX_SESSIONS).
 *   - The CC PD does not initiate communication; it only responds to PPCs.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define CC_PD_CH_CONTROLLER  CH_CC_PD

/* ─── Configuration ──────────────────────────────────────────────────────── */
#define CC_MAX_SESSIONS         8u
#define CC_SESSION_TIMEOUT_TICKS  5000u  /* ~50 seconds at 100 Hz */
#define CC_MAX_CMD_BYTES        4096u
#define CC_MAX_RESP_BYTES       4096u

/* ─── Command types ──────────────────────────────────────────────────────── */

#define CC_CMD_TYPE_QUERY   0x01u   /* read-only status query */
#define CC_CMD_TYPE_ACTION  0x02u   /* mutating action */
#define CC_CMD_TYPE_STREAM  0x03u   /* subscribe to event stream (future) */

/* ─── Session state ──────────────────────────────────────────────────────── */

#define CC_SESSION_STATE_CONNECTED  0
#define CC_SESSION_STATE_IDLE       1
#define CC_SESSION_STATE_BUSY       2
#define CC_SESSION_STATE_EXPIRED    3

/* ─── Request structs ────────────────────────────────────────────────────── */

struct cc_req_connect {
    uint32_t client_badge;      /* caller's seL4 badge / capability identifier */
    uint32_t flags;             /* CC_CONNECT_FLAG_* */
};

#define CC_CONNECT_FLAG_JSON   (1u << 0)  /* prefer JSON-encoded responses */
#define CC_CONNECT_FLAG_BINARY (1u << 1)  /* prefer binary-encoded responses */

struct cc_req_disconnect {
    uint32_t session_id;
};

struct cc_req_send {
    uint32_t session_id;
    uint32_t cmd_type;          /* CC_CMD_TYPE_* */
    uint32_t len;               /* command bytes in cc_shmem */
};

struct cc_req_recv {
    uint32_t session_id;
    uint32_t max;               /* max response bytes to return */
};

struct cc_req_status {
    uint32_t session_id;
};

struct cc_req_list {
    uint32_t max_entries;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct cc_reply_connect {
    uint32_t ok;
    uint32_t session_id;
};

struct cc_reply_disconnect {
    uint32_t ok;
};

struct cc_reply_send {
    uint32_t ok;
    uint32_t resp_pending;      /* 1 = response ready for MSG_CC_RECV */
};

struct cc_reply_recv {
    uint32_t ok;
    uint32_t len;               /* response bytes in cc_shmem */
};

struct cc_reply_status {
    uint32_t ok;
    uint32_t state;             /* CC_SESSION_STATE_* */
    uint32_t pending_responses; /* number of queued responses */
    uint32_t ticks_since_active;
};

struct cc_reply_list {
    uint32_t ok;
    uint32_t count;             /* entries written to cc_shmem */
};

/* ─── Shmem layout: session info entry ──────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t session_id;
    uint32_t state;
    uint32_t client_badge;
    uint32_t ticks_since_active;
} cc_session_info_t;

/* ─── MSG_CC_ATTACH_FRAMEBUFFER ──────────────────────────────────────────── */

/*
 * Subscribe the calling channel to framebuffer FRAME_READY notifications.
 * Sent to framebuffer_pd (not cc_pd) by external subscribers (agentctl,
 * remote desktop sessions).  After attaching, the subscriber's notified()
 * is called on every MSG_FB_FLIP for the specified framebuffer handle.
 * The subscriber reads pixel data from the shared fb_shmem region.
 *
 * MR1 = fb_handle (returned by MSG_FB_CREATE)
 * Returns: MR0 = FB_OK on success, FB_ERR_* on failure
 */
struct cc_req_attach_framebuffer {
    uint32_t fb_handle;     /* framebuffer handle from MSG_FB_CREATE */
    uint32_t _reserved[3];
};

struct cc_reply_attach_framebuffer {
    uint32_t ok;            /* FB_OK on success */
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum cc_error {
    CC_OK                   = 0,
    CC_ERR_NO_SESSIONS      = 1,  /* CC_MAX_SESSIONS reached */
    CC_ERR_BAD_SESSION      = 2,
    CC_ERR_EXPIRED          = 3,
    CC_ERR_CMD_TOO_LARGE    = 4,
    CC_ERR_NO_RESPONSE      = 5,  /* RECV with no pending response */
};
