/*
 * agentOS NetServer IPC Contract — interface.h
 *
 * Formal seL4 IPC API contract for the NetServer protection domain.
 * NetServer manages per-application virtual NICs and provides the TCP/IP
 * stack interface for all agents.  Agents never hold raw network capabilities;
 * all network I/O is mediated through this passive server.
 *
 * Architecture:
 *   - Up to 16 virtual NICs (vNICs), one per application or worker
 *   - Each vNIC maps to a 16KB slot in the net_packet_shmem MR (256KB total)
 *   - TX/RX rings live in the per-vNIC slot header (net_vnic_ring_t, 32 bytes)
 *   - ACL enforcement on every send (outbound, inbound, internet flags)
 *   - MAC addresses: locally administered, 02:00:00:00:00:NN
 *   - smoltcp TCP/IP integration: marked SMOLTCP_INTEGRATION_POINT in source
 *
 * Shared memory layout (net_packet_shmem, 256KB = NET_SHMEM_TOTAL):
 *   Slot N at byte offset N * NET_SHMEM_SLOT_SIZE (N = 0..15)
 *   Slot layout:
 *     [0..1023]      net_vnic_ring_t header (magic, counters, ring indices)
 *     [1024..16383]  packet data ring (15KB usable)
 *
 * This header is the canonical copy of the contract.  The low-level
 * OP_NET_* opcodes and NET_ERR_* codes are defined in net_server.h;
 * the packed IPC structs here are the formal wire protocol.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Version ─────────────────────────────────────────────────────────────── */

#define NET_SERVER_INTERFACE_VERSION  1

/* ── Limits ──────────────────────────────────────────────────────────────── */

#define NET_MAX_VNICS           16u
#define NET_MAX_BOUND_PORTS     8u
#define NET_SHMEM_TOTAL         0x40000u    /* 256KB */
#define NET_SHMEM_SLOT_SIZE     0x4000u     /* 16KB per slot */
#define NET_SHMEM_HDR_SIZE      1024u
#define NET_SHMEM_DATA_SIZE     (NET_SHMEM_SLOT_SIZE - NET_SHMEM_HDR_SIZE)
#define NET_SHMEM_BASE_VADDR    0x5000000u

/* ── vNIC ring header magic ──────────────────────────────────────────────── */

#define NET_VNIC_MAGIC          0x564E4943u  /* "VNIC" */

/* ── ACL flags ───────────────────────────────────────────────────────────── */

#define NET_ACL_ALLOW_OUTBOUND  (1u << 0)
#define NET_ACL_ALLOW_INBOUND   (1u << 1)
#define NET_ACL_INTERNET        (1u << 2)

/* ── Protocol identifiers ────────────────────────────────────────────────── */

#define NET_PROTO_TCP           0u
#define NET_PROTO_UDP           1u

/* ── Opcodes (match OP_NET_* in net_server.h exactly) ───────────────────── */

#define NET_OP_VNIC_CREATE      0xB0u
#define NET_OP_VNIC_DESTROY     0xB1u
#define NET_OP_VNIC_SEND        0xB2u
#define NET_OP_VNIC_RECV        0xB3u
#define NET_OP_BIND             0xB4u
#define NET_OP_CONNECT          0xB5u
#define NET_OP_STATUS           0xB6u
#define NET_OP_SET_ACL          0xB7u
#define NET_OP_HEALTH           0xB8u
#define NET_OP_CONN_STATE       0xB9u
#define NET_OP_TCP_CLOSE        0xBAu
#define NET_OP_HTTP_POST        0x500u   /* high-level HTTP POST proxy */

/* ── Error codes (match NET_ERR_* in net_server.h exactly) ──────────────── */

#define NET_ERR_OK              0u
#define NET_ERR_NO_VNICS        1u   /* vNIC table full */
#define NET_ERR_NOT_FOUND       2u   /* vnic_id not registered */
#define NET_ERR_PERM            3u   /* capability or ACL denied */
#define NET_ERR_STUB            4u   /* smoltcp not yet integrated */
#define NET_ERR_INVAL           5u   /* invalid argument */

/* ── vNIC ring header (at start of each 16KB shmem slot) ────────────────── */

typedef struct net_vnic_ring {
    uint32_t magic;      /* NET_VNIC_MAGIC */
    uint32_t vnic_id;
    uint32_t tx_head;    /* producer write index (app writes TX here) */
    uint32_t tx_tail;    /* consumer read index (NetServer reads TX) */
    uint32_t rx_head;    /* producer write index (NetServer writes RX) */
    uint32_t rx_tail;    /* consumer read index (app reads RX) */
    uint32_t tx_drops;
    uint32_t rx_drops;
} __attribute__((packed)) net_vnic_ring_t;   /* 32 bytes */

/* ── Request / reply structs ─────────────────────────────────────────────── */

/*
 * NET_OP_VNIC_CREATE (0xB0)
 *
 * Create a new virtual NIC.
 *   requested_vnic_id: use 0xFF for auto-assign
 *   cap_classes: must include CAP_CLASS_NET (bit 1) else NET_ERR_PERM
 *   caller_pd_id: recorded as vNIC owner for ACL
 *
 * Reply:
 *   status, assigned_vnic_id, shmem_slot_offset
 *   (shmem_slot_offset is the byte offset into net_packet_shmem)
 */
typedef struct net_vnic_create_req {
    uint32_t opcode;                /* NET_OP_VNIC_CREATE */
    uint32_t requested_vnic_id;     /* 0xFF = auto-assign */
    uint32_t cap_classes;           /* CAP_CLASS_* bitmask */
    uint32_t caller_pd_id;
} __attribute__((packed)) net_vnic_create_req_t;

typedef struct net_vnic_create_rep {
    uint32_t status;                /* NET_ERR_* */
    uint32_t assigned_vnic_id;
    uint32_t shmem_slot_offset;     /* byte offset into net_packet_shmem */
} __attribute__((packed)) net_vnic_create_rep_t;

/*
 * NET_OP_VNIC_DESTROY (0xB1)
 *
 * Destroy a vNIC and free its shmem slot.
 */
typedef struct net_vnic_destroy_req {
    uint32_t opcode;                /* NET_OP_VNIC_DESTROY */
    uint32_t vnic_id;
} __attribute__((packed)) net_vnic_destroy_req_t;

typedef struct net_vnic_destroy_rep {
    uint32_t status;
} __attribute__((packed)) net_vnic_destroy_rep_t;

/*
 * NET_OP_VNIC_SEND (0xB2)
 *
 * Transmit a packet from a vNIC.
 *   packet_offset: byte offset into net_packet_shmem (not the slot offset)
 *   packet_len: bytes of the packet
 * ACL: NET_ACL_ALLOW_OUTBOUND must be set for this vNIC.
 * Loopback (127.x.x.x) is routed to the appropriate local vNIC.
 */
typedef struct net_vnic_send_req {
    uint32_t opcode;                /* NET_OP_VNIC_SEND */
    uint32_t vnic_id;
    uint32_t packet_offset;
    uint32_t packet_len;
} __attribute__((packed)) net_vnic_send_req_t;

typedef struct net_vnic_send_rep {
    uint32_t status;
    uint32_t bytes_sent;
} __attribute__((packed)) net_vnic_send_rep_t;

/*
 * NET_OP_VNIC_RECV (0xB3)
 *
 * Receive a packet from a vNIC into the caller's shmem buffer.
 *   buf_offset: byte offset into net_packet_shmem for the receive buffer
 *   buf_len: maximum bytes to receive
 * Returns bytes_received = 0 in stub (smoltcp not yet integrated).
 */
typedef struct net_vnic_recv_req {
    uint32_t opcode;                /* NET_OP_VNIC_RECV */
    uint32_t vnic_id;
    uint32_t buf_offset;
    uint32_t buf_len;
} __attribute__((packed)) net_vnic_recv_req_t;

typedef struct net_vnic_recv_rep {
    uint32_t status;
    uint32_t bytes_received;
} __attribute__((packed)) net_vnic_recv_rep_t;

/*
 * NET_OP_BIND (0xB4)
 *
 * Bind a port to a vNIC.
 *   protocol: NET_PROTO_TCP=0 or NET_PROTO_UDP=1
 */
typedef struct net_bind_req {
    uint32_t opcode;                /* NET_OP_BIND */
    uint32_t vnic_id;
    uint32_t port;
    uint32_t protocol;
} __attribute__((packed)) net_bind_req_t;

typedef struct net_bind_rep {
    uint32_t status;
} __attribute__((packed)) net_bind_rep_t;

/*
 * NET_OP_CONNECT (0xB5)
 *
 * Connect a vNIC to a remote endpoint.
 *   dest_ip: packed uint32 big-endian (network byte order)
 *   dest_port: 1..65535
 *   protocol: NET_PROTO_TCP or NET_PROTO_UDP
 * Returns NET_ERR_STUB until smoltcp is integrated.
 */
typedef struct net_connect_req {
    uint32_t opcode;                /* NET_OP_CONNECT */
    uint32_t vnic_id;
    uint32_t dest_ip;               /* big-endian packed IPv4 */
    uint32_t dest_port;
    uint32_t protocol;
} __attribute__((packed)) net_connect_req_t;

typedef struct net_connect_rep {
    uint32_t status;
    uint32_t conn_id;               /* 0 in stub */
} __attribute__((packed)) net_connect_rep_t;

/*
 * NET_OP_STATUS (0xB6)
 *
 * Query vNIC or global status.
 *   vnic_id: use 0xFFFFFFFF for global status
 */
typedef struct net_status_req {
    uint32_t opcode;                /* NET_OP_STATUS */
    uint32_t vnic_id;               /* 0xFFFFFFFF = global */
} __attribute__((packed)) net_status_req_t;

typedef struct net_status_rep {
    uint32_t status;
    uint32_t active_vnics;
    uint32_t rx_packets;
    uint32_t tx_packets;
} __attribute__((packed)) net_status_rep_t;

/*
 * NET_OP_SET_ACL (0xB7)
 *
 * Update ACL flags for a vNIC.
 *   acl_flags: NET_ACL_* bitmask
 */
typedef struct net_set_acl_req {
    uint32_t opcode;                /* NET_OP_SET_ACL */
    uint32_t vnic_id;
    uint32_t acl_flags;
} __attribute__((packed)) net_set_acl_req_t;

typedef struct net_set_acl_rep {
    uint32_t status;
} __attribute__((packed)) net_set_acl_rep_t;

/*
 * NET_OP_HEALTH (0xB8)
 *
 * Liveness probe.
 */
typedef struct net_health_req {
    uint32_t opcode;                /* NET_OP_HEALTH */
} __attribute__((packed)) net_health_req_t;

typedef struct net_health_rep {
    uint32_t status;
    uint32_t active_vnic_count;
    uint32_t version;               /* NET_SERVER_INTERFACE_VERSION */
} __attribute__((packed)) net_health_rep_t;

/*
 * NET_OP_HTTP_POST (0x500)
 *
 * High-level HTTP POST proxy to the agentOS bridge at 10.0.2.2:8790.
 * Used by libagent's aos_http_post(); agents do not manage raw TCP.
 *
 * The URL and body must be placed in the vibe_staging shared MR
 * before calling (offsets are relative to vibe_staging_vaddr).
 *
 * Returns the HTTP status code in response_status (200, 422, 503, etc.)
 * or 0 on TCP error.  The response body is written to the staging MR.
 */
typedef struct net_http_post_req {
    uint32_t opcode;                /* NET_OP_HTTP_POST */
    uint32_t url_staging_offset;
    uint32_t url_len;
    uint32_t body_staging_offset;
    uint32_t body_len;
} __attribute__((packed)) net_http_post_req_t;

typedef struct net_http_post_rep {
    uint32_t http_status;           /* HTTP status code or 0 on error */
    uint32_t resp_staging_offset;
    uint32_t resp_len;
} __attribute__((packed)) net_http_post_rep_t;
