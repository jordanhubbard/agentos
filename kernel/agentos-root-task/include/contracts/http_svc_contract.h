/*
 * HttpSvc IPC Contract
 *
 * The HttpSvc PD is the HTTP routing table for agentOS.  It does not serve
 * HTTP itself; it maps URL path prefixes to (app_id, vnic_id) pairs so that
 * the network service can forward incoming requests to the correct guest OS
 * application.  This is the control plane only — data plane forwarding is
 * handled in the network path without an IPC round-trip.
 *
 * Channel: CH_HTTP_SVC = 24 (from controller perspective)
 * Opcodes: OP_HTTP_REGISTER, OP_HTTP_UNREGISTER, OP_HTTP_DISPATCH,
 *          OP_HTTP_LIST, OP_HTTP_HEALTH
 *
 * Shmem: http_req_shmem
 *   Used by OP_HTTP_LIST to write an array of http_handler_entry_t records.
 *   OP_HTTP_REGISTER reads the path prefix from http_req_shmem if
 *   prefix_len exceeds the inline MR capacity.
 *
 * Invariants:
 *   - Path prefixes are NUL-terminated ASCII strings.
 *   - Prefix matching is longest-prefix-wins.
 *   - Registrations persist until OP_HTTP_UNREGISTER or the registering
 *     app_id exits.
 *   - handler_id is opaque; HttpSvc owns the namespace.
 *   - HTTP_SVC_VERSION is incremented when the wire format changes.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define CH_HTTP_SVC      24      /* controller → http_svc */

/* ─── Protocol version ───────────────────────────────────────────────────── */
#define HTTP_SVC_VERSION  1u

/* ─── Opcodes ─────────────────────────────────────────────────────────────── */
#define OP_HTTP_REGISTER    0x01
#define OP_HTTP_UNREGISTER  0x02
#define OP_HTTP_DISPATCH    0x03
#define OP_HTTP_LIST        0x04
#define OP_HTTP_HEALTH      0x05

/* ─── Inline prefix size ─────────────────────────────────────────────────── */
#define HTTP_PREFIX_MR_WORDS   8u   /* MR4..MR11 carry prefix inline */

/* ─── Request structs ────────────────────────────────────────────────────── */

struct http_svc_req_register {
    uint32_t op;                           /* OP_HTTP_REGISTER */
    uint32_t app_id;                       /* registering application */
    uint32_t vnic_id;                      /* virtual NIC to forward to */
    uint32_t prefix_len;                   /* byte length of path prefix (incl. NUL) */
    uint32_t prefix[HTTP_PREFIX_MR_WORDS]; /* inline prefix; overflow uses shmem */
};

struct http_svc_req_unregister {
    uint32_t op;                 /* OP_HTTP_UNREGISTER */
    uint32_t handler_id_or_app_id; /* handler_id to remove, or app_id to
                                      remove all handlers for that app */
};

struct http_svc_req_dispatch {
    uint32_t op;                           /* OP_HTTP_DISPATCH */
    uint32_t path[HTTP_PREFIX_MR_WORDS];   /* request path (NUL-terminated, inline) */
};

struct http_svc_req_list {
    uint32_t op;                 /* OP_HTTP_LIST */
};

struct http_svc_req_health {
    uint32_t op;                 /* OP_HTTP_HEALTH */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct http_svc_reply_register {
    uint32_t ok;                 /* HTTP_OK or error code */
    uint32_t handler_id;         /* assigned handler identifier; 0 on error */
};

struct http_svc_reply_unregister {
    uint32_t ok;                 /* HTTP_OK or error code */
};

struct http_svc_reply_dispatch {
    uint32_t ok;                 /* HTTP_OK or error code */
    uint32_t app_id;             /* app to forward to */
    uint32_t vnic_id;            /* vnic to use */
    uint32_t handler_id;         /* matched handler */
};

struct http_svc_reply_list {
    uint32_t ok;                 /* HTTP_OK or error code */
    uint32_t count;              /* http_handler_entry_t records in shmem */
};

struct http_svc_reply_health {
    uint32_t ok;                 /* HTTP_OK or error code */
    uint32_t active_routes;      /* number of registered handlers */
    uint32_t version;            /* HTTP_SVC_VERSION */
};

/* ─── Shmem layout: handler entry ────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t handler_id;
    uint32_t app_id;
    uint32_t vnic_id;
    uint8_t  prefix[64];         /* NUL-terminated path prefix */
} http_handler_entry_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum http_svc_error {
    HTTP_OK              = 0,
    HTTP_ERR_INVAL       = 1,    /* bad argument */
    HTTP_ERR_NO_SLOTS    = 2,    /* handler table full */
    HTTP_ERR_NOT_FOUND   = 3,    /* no handler matches path */
};
