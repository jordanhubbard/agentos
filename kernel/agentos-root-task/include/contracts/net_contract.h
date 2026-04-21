/*
 * Network Device PD IPC Contract
 *
 * The net_pd owns the network interface exclusively via seL4 device frame
 * capabilities.  It provides an OS-neutral packet I/O API.
 *
 * Channel: CH_NET_PD (see agentos.h)
 * Opcodes: MSG_NET_OPEN, MSG_NET_CLOSE, MSG_NET_SEND, MSG_NET_RECV,
 *          MSG_NET_DEV_STATUS, MSG_NET_CONFIGURE, MSG_NET_FILTER_ADD,
 *          MSG_NET_FILTER_REMOVE (see agentos.h)
 *
 * Invariants:
 *   - MSG_NET_OPEN returns a handle; all subsequent calls use it.
 *   - One handle per interface; multiple clients share via the client table
 *     (each gets their own RX ring in net_shmem).
 *   - MSG_NET_SEND: Ethernet frame (up to 1514 bytes) placed in net_shmem.
 *   - MSG_NET_RECV: Zero-copy — returns offset into net_shmem where frame lives.
 *   - MSG_NET_FILTER_ADD installs a BPF-style packet filter rule;
 *     rules apply to inbound frames for this handle only.
 *   - Guest OSes receive packets via a dedicated shared memory ring (one per guest).
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define NET_PD_CH_CONTROLLER  CH_NET_PD

/* ─── Configuration ──────────────────────────────────────────────────────── */
#define NET_MAX_FRAME_BYTES   1514u
#define NET_MAX_CLIENTS       16u
#define NET_MAX_FILTERS       32u

/* ─── Protocol identifiers (MSG_NET_SOCKET_OPEN / MSG_NET_SOCKET_CONNECT) ── */
#define NET_PROTO_TCP         0u
#define NET_PROTO_UDP         1u

/* ─── Request structs ────────────────────────────────────────────────────── */

struct net_req_open {
    uint32_t iface_id;          /* 0 = first interface */
};

struct net_req_close {
    uint32_t handle;
};

struct net_req_send {
    uint32_t handle;
    uint32_t len;               /* frame length in net_shmem */
};

struct net_req_recv {
    uint32_t handle;
    uint32_t max;               /* max bytes to return */
};

struct net_req_dev_status {
    uint32_t handle;
};

struct net_req_configure {
    uint32_t handle;
    uint32_t mtu;               /* 0 = use default */
    uint32_t flags;             /* NET_CFG_FLAG_* */
};

#define NET_CFG_FLAG_PROMISC    (1u << 0)
#define NET_CFG_FLAG_ALLMULTI   (1u << 1)

struct net_req_filter_add {
    uint32_t handle;
    uint32_t filter_len;        /* filter rule in net_shmem (BPF-style bytecodes) */
};

struct net_req_filter_remove {
    uint32_t handle;
    uint32_t filter_id;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct net_reply_open {
    uint32_t ok;
    uint32_t handle;
    uint8_t  mac[6];            /* interface MAC address */
    uint8_t  _pad[2];
};

struct net_reply_close {
    uint32_t ok;
};

struct net_reply_send {
    uint32_t ok;
};

struct net_reply_recv {
    uint32_t ok;
    uint32_t len;               /* 0 = no frame available */
    uint32_t shmem_offset;      /* offset into net_shmem where frame starts */
};

struct net_reply_dev_status {
    uint32_t ok;
    uint32_t link_up;           /* 1 = link established */
    uint64_t rx_pkts;
    uint64_t tx_pkts;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint32_t rx_errors;
    uint32_t tx_errors;
};

struct net_reply_configure {
    uint32_t ok;
};

struct net_reply_filter_add {
    uint32_t ok;
    uint32_t filter_id;
};

struct net_reply_filter_remove {
    uint32_t ok;
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum net_error {
    NET_OK                  = 0,
    NET_ERR_NO_SLOTS        = 1,
    NET_ERR_BAD_HANDLE      = 2,
    NET_ERR_BAD_IFACE       = 3,
    NET_ERR_LINK_DOWN       = 4,
    NET_ERR_FRAME_TOO_LARGE = 5,
    NET_ERR_FILTER_FULL     = 6,
    NET_ERR_BAD_FILTER_ID   = 7,
};

/* ─── Socket-level extension (MSG_NET_SOCKET_* opcodes 0x2109–0x210F) ─────
 *
 * These extend net_pd beyond raw Ethernet frame I/O with a BSD socket-like
 * API backed by the internal lwIP TCP/IP stack.  Guest OSes and agents use
 * these operations to obtain TCP/UDP connections without touching lwIP directly.
 *
 * All socket opcodes are dispatched by net_pd on channel CH_NET_PD (68).
 * Opcode values are defined in agentos.h (MSG_NET_SOCKET_* range 0x2109–0x210F).
 * ────────────────────────────────────────────────────────────────────────── */

/* Socket option IDs (MSG_NET_SOCKET_SET_OPT) */
#define NET_SOCKOPT_REUSEADDR  0u   /* SO_REUSEADDR */
#define NET_SOCKOPT_NODELAY    1u   /* TCP_NODELAY */
#define NET_SOCKOPT_KEEPALIVE  2u   /* SO_KEEPALIVE */

/* Socket states (returned in replies — for informational use) */
#define NET_SOCK_FREE       0u
#define NET_SOCK_BOUND      1u
#define NET_SOCK_CONNECTING 2u
#define NET_SOCK_CONNECTED  3u
#define NET_SOCK_LISTENING  4u
#define NET_SOCK_CLOSED     5u

/* ── MSG_NET_SOCKET_OPEN (0x2109) — open a TCP or UDP socket ────────────── */
struct net_req_socket_open {
    uint32_t proto;             /* NET_PROTO_TCP=0, NET_PROTO_UDP=1 */
};

struct net_reply_socket_open {
    uint32_t ok;
    uint32_t sock_handle;       /* opaque handle for subsequent calls */
    uint32_t shmem_slot_offset; /* byte offset into net_pd_shmem for data I/O */
};

/* ── MSG_NET_SOCKET_CLOSE (0x210A) — close a socket ─────────────────────── */
struct net_req_socket_close {
    uint32_t sock_handle;
};

struct net_reply_socket_close {
    uint32_t ok;
};

/* ── MSG_NET_SOCKET_CONNECT (0x210B) — initiate a TCP connection ─────────── */
struct net_req_socket_connect {
    uint32_t sock_handle;
    uint32_t dest_ip;           /* IPv4 destination, big-endian */
    uint32_t dest_port;         /* destination TCP port (1–65535) */
};

struct net_reply_socket_connect {
    uint32_t ok;                /* NET_OK = connection initiated (async) */
};

/* ── MSG_NET_SOCKET_BIND (0x210C) — bind to a local port ────────────────── */
struct net_req_socket_bind {
    uint32_t sock_handle;
    uint32_t local_port;        /* 1–65535 */
};

struct net_reply_socket_bind {
    uint32_t ok;
};

/* ── MSG_NET_SOCKET_LISTEN (0x210D) — listen for incoming connections ────── */
struct net_req_socket_listen {
    uint32_t sock_handle;       /* must be bound (MSG_NET_SOCKET_BIND first) */
};

struct net_reply_socket_listen {
    uint32_t ok;
};

/* ── MSG_NET_SOCKET_ACCEPT (0x210E) — accept a pending connection ─────────── */
struct net_req_socket_accept {
    uint32_t sock_handle;       /* the listening socket handle */
};

struct net_reply_socket_accept {
    uint32_t ok;
    uint32_t new_sock_handle;   /* 0 = no connection pending */
    uint32_t peer_ip;           /* remote IPv4, big-endian */
    uint32_t peer_port;
};

/* ── MSG_NET_SOCKET_SET_OPT (0x210F) — set a socket option ──────────────── */
struct net_req_socket_set_opt {
    uint32_t sock_handle;
    uint32_t option;            /* NET_SOCKOPT_* */
    uint32_t value;             /* 0 = disable, 1 = enable */
};

struct net_reply_socket_set_opt {
    uint32_t ok;
};
