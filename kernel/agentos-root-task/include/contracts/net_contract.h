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
