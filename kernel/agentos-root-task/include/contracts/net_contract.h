#pragma once
/* NET_PD contract — version 1
 * PD: net_pd | Source: src/net_pd.c | Channel: CH_NET_PD (62) from controller
 * Provides OS-neutral IPC network interface access. One handle per client interface claim.
 */
#include <stdint.h>
#include <stdbool.h>

#define NET_PD_CONTRACT_VERSION 1

/* ── Channel ─────────────────────────────────────────────────────────────── */
#define CH_NET_PD  62u

/* ── Opcodes (from agentos_msg_tag_t) ───────────────────────────────────── */
#define NET_OP_OPEN          0x1010u
#define NET_OP_CLOSE         0x1011u
#define NET_OP_SEND          0x1012u
#define NET_OP_RECV          0x1013u
#define NET_OP_STATUS        0x1014u
#define NET_OP_CONFIGURE     0x1015u
#define NET_OP_FILTER_ADD    0x1016u
#define NET_OP_FILTER_REMOVE 0x1017u

#define NET_MAX_HANDLES      4u
#define NET_FRAME_MAX        1514u  /* max Ethernet frame (no VLAN) */

/* ── Request structs ─────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t op;           /* NET_OP_OPEN */
    uint32_t iface_index;  /* physical interface index (0-based) */
} net_req_open_t;

typedef struct __attribute__((packed)) {
    uint32_t op;           /* NET_OP_CLOSE */
    uint32_t handle;
} net_req_close_t;

typedef struct __attribute__((packed)) {
    uint32_t op;           /* NET_OP_SEND */
    uint32_t handle;
    uint32_t shmem_offset; /* offset into net_shmem containing frame */
    uint32_t len;          /* frame length in bytes, max NET_FRAME_MAX */
} net_req_send_t;

typedef struct __attribute__((packed)) {
    uint32_t op;           /* NET_OP_RECV */
    uint32_t handle;
    uint32_t shmem_offset; /* offset into net_shmem for rx frame */
    uint32_t max_len;      /* max bytes to copy */
} net_req_recv_t;

typedef struct __attribute__((packed)) {
    uint32_t op;           /* NET_OP_STATUS */
    uint32_t handle;
} net_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t op;           /* NET_OP_CONFIGURE */
    uint32_t handle;
    uint32_t flags;        /* promisc, multicast, etc. */
    uint32_t mtu;
} net_req_configure_t;

typedef struct __attribute__((packed)) {
    uint32_t op;           /* NET_OP_FILTER_ADD */
    uint32_t handle;
    uint32_t rule_id;
} net_req_filter_add_t;

typedef struct __attribute__((packed)) {
    uint32_t op;           /* NET_OP_FILTER_REMOVE */
    uint32_t handle;
    uint32_t rule_id;
} net_req_filter_remove_t;

/* ── Reply structs ───────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t result;       /* 0 = ok */
    uint32_t handle;       /* assigned handle */
} net_reply_open_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t bytes_sent;
} net_reply_send_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t bytes_recv;
} net_reply_recv_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t link_up;      /* 1 = link, 0 = no link */
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_errors;
    uint32_t tx_errors;
} net_reply_status_t;

/* ── Error codes ─────────────────────────────────────────────────────────── */
typedef enum {
    NET_OK               = 0,
    NET_ERR_NO_HANDLE    = 1,  /* no free handle slots */
    NET_ERR_BAD_HANDLE   = 2,  /* invalid handle */
    NET_ERR_BAD_LEN      = 3,  /* frame too large */
    NET_ERR_HW           = 4,  /* hardware / driver error */
    NET_ERR_NOT_IMPL     = 5,  /* operation not yet implemented */
    NET_ERR_NO_PKT       = 6,  /* no packet available on recv */
} net_error_t;

/* ── Invariants ──────────────────────────────────────────────────────────
 * - Each handle corresponds to an exclusive claim on one physical interface.
 * - shmem_offset must be 4-byte aligned.
 * - Zero-copy recv: net_pd writes frame into shmem at shmem_offset; caller reads after reply.
 * - NET_FRAME_MAX enforced; larger frames silently dropped.
 */
