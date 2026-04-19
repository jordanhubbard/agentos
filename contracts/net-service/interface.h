/*
 * contracts/net-service/interface.h — Network Service Generic Device Interface
 *
 * // STATUS: IMPLEMENTED
 *
 * This is the canonical contract for the net-service device service in agentOS.
 * The concrete implementation lives in kernel/agentos-root-task/src/net_server.c,
 * with the lwIP shim in lwip_shim.c.
 *
 * The net-service provides virtual NIC (vNIC) creation, ACL-gated packet
 * send/receive, port binding, and TCP connection management for all agents and
 * guest operating systems.  Every guest OS and VMM MUST use this service for
 * network access rather than configuring a physical or virtio-net NIC directly.
 *
 * IPC transport:
 *   - Protected procedure call (PPC) via seL4 Microkit.
 *   - MR0 = opcode (NET_SVC_OP_*)
 *   - MR1..MR6 = arguments (opcode-specific, see per-op comments below)
 *   - Reply: MR0 = status (NET_SVC_ERR_*), MR1..MR6 = result fields
 *
 * Shared-memory packet ring layout (net_packet_shmem, 256 KB):
 *   Each vNIC occupies NET_SVC_SLOT_SIZE (16 KB) starting at
 *   slot_index * NET_SVC_SLOT_SIZE.  The first NET_SVC_HDR_SIZE bytes of each
 *   slot contain a net_svc_vnic_ring_t header followed by packet data.
 *
 * Capability grant:
 *   vm_manager.c grants a PPC capability to the net-service endpoint and a
 *   read-write mapping of the relevant packet-shmem slot at guest OS creation
 *   time.  The guest discovers its vNIC id via the NET_SVC_OP_CONNECT reply.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

/* ── Interface version ──────────────────────────────────────────────────── */
#define NET_SVC_INTERFACE_VERSION       1

/* ── Geometry / limits ──────────────────────────────────────────────────── */
#define NET_SVC_MAX_VNICS               16u
#define NET_SVC_MAX_BOUND_PORTS         8u
#define NET_SVC_SHMEM_TOTAL             0x40000u   /* 256 KB */
#define NET_SVC_SLOT_SIZE               0x4000u    /* 16 KB per vNIC slot */
#define NET_SVC_HDR_SIZE                1024u      /* ring header bytes */
#define NET_SVC_DATA_SIZE               (NET_SVC_SLOT_SIZE - NET_SVC_HDR_SIZE)

/* ── Protocol identifiers (NET_SVC_OP_BIND / NET_SVC_OP_CONNECT) ────────── */
#define NET_SVC_PROTO_TCP               0u
#define NET_SVC_PROTO_UDP               1u

/* ── ACL flags (used with NET_SVC_OP_CONNECT and GET_STATS) ─────────────── */
#define NET_SVC_ACL_ALLOW_OUTBOUND      (1u << 0)
#define NET_SVC_ACL_ALLOW_INBOUND       (1u << 1)
#define NET_SVC_ACL_INTERNET            (1u << 2)

/* ── vNIC ring header (at offset 0 in each 16 KB slot) ──────────────────── */
#define NET_SVC_VNIC_MAGIC              0x564E4943u  /* "VNIC" */

typedef struct __attribute__((packed)) {
    uint32_t magic;      /* NET_SVC_VNIC_MAGIC */
    uint32_t vnic_id;
    uint32_t tx_head;    /* write index — client writes TX packets here */
    uint32_t tx_tail;    /* read index  — net-service reads TX packets  */
    uint32_t rx_head;    /* write index — net-service writes RX packets */
    uint32_t rx_tail;    /* read index  — client reads RX packets       */
    uint32_t tx_drops;   /* packets dropped: TX ring full */
    uint32_t rx_drops;   /* packets dropped: RX ring full */
} net_svc_vnic_ring_t;   /* 32 bytes */

/* ── Opcodes (MR0) ──────────────────────────────────────────────────────── */

/*
 * NET_SVC_OP_CONNECT (0xB0)
 * Allocate a vNIC and connect it to the network service.
 * Maps to the underlying OP_NET_VNIC_CREATE in net_server.h.
 *
 * Request:
 *   MR1 = requested_vnic_id  — 0xFF = auto-assign
 *   MR2 = cap_classes        — capability class bitmask (must include CAP_CLASS_NET)
 *   MR3 = caller_pd_id       — recorded as owner for ACL
 * Reply:
 *   MR0 = status
 *   MR1 = assigned_vnic_id
 *   MR2 = shmem_slot_offset  — byte offset into net_packet_shmem for this vNIC
 */
#define NET_SVC_OP_CONNECT              0xB0u

/*
 * NET_SVC_OP_DISCONNECT (0xB1)
 * Release a vNIC and free its slot.
 *
 * Request:
 *   MR1 = vnic_id
 * Reply:
 *   MR0 = status
 */
#define NET_SVC_OP_DISCONNECT           0xB1u

/*
 * NET_SVC_OP_SEND (0xB2)
 * Transmit a packet from a vNIC.
 *
 * The caller places the packet in net_packet_shmem at the given offset
 * (within its assigned slot's data region) before issuing this call.
 *
 * Request:
 *   MR1 = vnic_id
 *   MR2 = packet_offset   — byte offset into net_packet_shmem
 *   MR3 = packet_len      — bytes
 * Reply:
 *   MR0 = status
 *   MR1 = bytes_sent
 */
#define NET_SVC_OP_SEND                 0xB2u

/*
 * NET_SVC_OP_RECV (0xB3)
 * Receive a packet from the vNIC's RX ring into a caller buffer.
 *
 * The net-service writes received data into net_packet_shmem at buf_offset.
 *
 * Request:
 *   MR1 = vnic_id
 *   MR2 = buf_offset      — byte offset into net_packet_shmem
 *   MR3 = buf_len         — maximum bytes to receive
 * Reply:
 *   MR0 = status
 *   MR1 = bytes_received
 */
#define NET_SVC_OP_RECV                 0xB3u

/*
 * NET_SVC_OP_BIND (0xB4)
 * Bind a port to the vNIC (UDP or TCP listen socket).
 *
 * Request:
 *   MR1 = vnic_id
 *   MR2 = port            — 1..65535
 *   MR3 = protocol        — NET_SVC_PROTO_TCP or NET_SVC_PROTO_UDP
 * Reply:
 *   MR0 = status
 */
#define NET_SVC_OP_BIND                 0xB4u

/*
 * NET_SVC_OP_LISTEN (0xB5)
 * Mark a bound port as listening for inbound connections (TCP only).
 *
 * Request:
 *   MR1 = vnic_id
 *   MR2 = port
 *   MR3 = backlog         — maximum number of pending connections
 * Reply:
 *   MR0 = status
 */
#define NET_SVC_OP_LISTEN               0xB5u

/*
 * NET_SVC_OP_ACCEPT (0xB6)
 * Accept one pending inbound TCP connection on a listening port.
 *
 * Request:
 *   MR1 = vnic_id
 *   MR2 = port
 * Reply:
 *   MR0 = status
 *   MR1 = conn_id         — new connection identifier
 *   MR2 = remote_ip       — packed u32, big-endian network byte order
 *   MR3 = remote_port
 */
#define NET_SVC_OP_ACCEPT               0xB6u

/*
 * NET_SVC_OP_GET_STATS (0xB7)
 * Query vNIC or global network statistics.
 *
 * Request:
 *   MR1 = vnic_id         — 0xFFFFFFFF = global totals
 * Reply:
 *   MR0 = status
 *   MR1 = active_vnics
 *   MR2 = rx_packets      — for the queried vNIC (or system total)
 *   MR3 = tx_packets
 *   MR4 = rx_drops
 *   MR5 = tx_drops
 */
#define NET_SVC_OP_GET_STATS            0xB7u

/* ── Error / status codes (MR0 in replies) ──────────────────────────────── */
#define NET_SVC_ERR_OK                  0u   /* success */
#define NET_SVC_ERR_NO_VNICS            1u   /* vNIC table full */
#define NET_SVC_ERR_NOT_FOUND           2u   /* vnic_id not registered */
#define NET_SVC_ERR_PERM                3u   /* capability or ACL denied */
#define NET_SVC_ERR_STUB                4u   /* feature pending smoltcp integration */
#define NET_SVC_ERR_INVAL               5u   /* invalid argument */
#define NET_SVC_ERR_NO_PORT             6u   /* port not bound or already in use */
#define NET_SVC_ERR_CONN_REFUSED        7u   /* remote refused connection */
#define NET_SVC_ERR_TIMEOUT             8u   /* operation timed out */

/* ── Request / reply structs ─────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;
    uint32_t vnic_id;
    uint32_t arg1;    /* packet_offset / port / buf_offset / requested_vnic_id */
    uint32_t arg2;    /* packet_len / protocol / buf_len / cap_classes */
    uint32_t arg3;    /* backlog / caller_pd_id */
} net_svc_req_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* NET_SVC_ERR_* */
    uint32_t field1;          /* assigned_vnic_id / bytes_sent / bytes_received /
                                  conn_id / active_vnics / shmem_slot_offset */
    uint32_t field2;          /* shmem_slot_offset / rx_packets / remote_ip */
    uint32_t field3;          /* tx_packets / remote_port */
    uint32_t field4;          /* rx_drops */
    uint32_t field5;          /* tx_drops */
} net_svc_reply_t;
