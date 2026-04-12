/*
 * agentOS NetServer — Public Interface
 *
 * NetServer is a passive PD (priority 160) that manages per-application
 * virtual NICs and provides the network stack interface for all agents.
 *
 * Architecture:
 *   - Up to NET_MAX_VNICS (16) virtual NICs, one per application/worker
 *   - Each vNIC maps to a 16KB slot in net_packet_shmem (256KB total)
 *   - TX/RX rings live in the per-vNIC slot header (net_vnic_ring_t)
 *   - ACL enforcement is performed on every send
 *   - MAC addresses are locally-administered: 02:00:00:00:00:NN
 *
 * smoltcp integration:
 *   The TCP/IP stack is stubbed.  Every integration point is marked with:
 *     SMOLTCP_INTEGRATION_POINT
 *   To wire up smoltcp, implement the EthernetInterface callbacks at those
 *   marked sites and link against the smoltcp C bindings (or Rust FFI shim).
 *
 * Shared memory layout (net_packet_shmem, 256KB = 0x40000):
 *   Slot N lives at offset N * NET_SHMEM_SLOT_SIZE (N = 0..15).
 *   Within each slot:
 *     [0    .. 1023]  net_vnic_ring_t header (magic, counters, ring indices)
 *     [1024 .. 16383] packet data ring (15KB usable packet storage)
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Version ─────────────────────────────────────────────────────────────── */
#define NET_VERSION             1u

/* ── Limits ──────────────────────────────────────────────────────────────── */
#define NET_MAX_VNICS           16u   /* hard limit; matches shmem slot count */
#define NET_MAX_BOUND_PORTS     8u    /* bound ports per vNIC */

/* ── shmem slot geometry ─────────────────────────────────────────────────── */
#define NET_SHMEM_TOTAL         0x40000u          /* 256KB */
#define NET_SHMEM_SLOT_SIZE     0x4000u           /* 16KB per vNIC slot */
#define NET_SHMEM_HDR_SIZE      1024u             /* bytes reserved for ring header */
#define NET_SHMEM_DATA_SIZE     (NET_SHMEM_SLOT_SIZE - NET_SHMEM_HDR_SIZE)  /* 15KB */
#define NET_SHMEM_BASE_VADDR    0x5000000u        /* NetServer's mapping of net_packet_shmem */

/* ── vNIC ring header (at start of each 16KB slot) ───────────────────────── */
#define NET_VNIC_MAGIC          0x564E4943u       /* "VNIC" */

typedef struct {
    uint32_t magic;       /* NET_VNIC_MAGIC = 0x564E4943 */
    uint32_t vnic_id;
    uint32_t tx_head;     /* write index — app writes TX packets here */
    uint32_t tx_tail;     /* read index  — NetServer reads TX packets */
    uint32_t rx_head;     /* write index — NetServer writes RX packets */
    uint32_t rx_tail;     /* read index  — app reads RX packets */
    uint32_t tx_drops;    /* packets dropped: TX ring full */
    uint32_t rx_drops;    /* packets dropped: RX ring full */
} net_vnic_ring_t;        /* 32 bytes */

/* ── vNIC ACL flags ──────────────────────────────────────────────────────── */
#define NET_ACL_ALLOW_OUTBOUND  (1u << 0)  /* may send packets out */
#define NET_ACL_ALLOW_INBOUND   (1u << 1)  /* may receive inbound packets */
#define NET_ACL_INTERNET        (1u << 2)  /* may reach non-loopback addresses */

/* ── Protocol identifiers (OP_NET_BIND / OP_NET_CONNECT) ────────────────── */
#define NET_PROTO_TCP           0u
#define NET_PROTO_UDP           1u

/* ── Channel IDs from NetServer's own perspective ─────────────────────────── */
#define NET_CH_CONTROLLER       0u   /* pp=true inbound from controller */
#define NET_CH_INIT_AGENT       1u   /* pp=true inbound from init_agent */
#define NET_CH_WORKER_0         2u   /* pp=true inbound from worker_0   */
#define NET_CH_WORKER_1         3u
#define NET_CH_WORKER_2         4u
#define NET_CH_WORKER_3         5u
#define NET_CH_WORKER_4         6u
#define NET_CH_WORKER_5         7u
#define NET_CH_WORKER_6         8u
#define NET_CH_WORKER_7         9u
#define NET_CH_APP_MANAGER      10u  /* pp=true inbound — creates vNICs */
#define NET_CH_TIMER            12u  /* 10ms periodic tick from controller (ch 11 used by wg_net) */

/* Channel IDs reaching NetServer from other PDs' perspectives */
#define NET_CH_FROM_CONTROLLER  110u
#define NET_CH_FROM_INIT_AGENT  10u
#define NET_CH_FROM_WORKER      14u
#define NET_CH_FROM_APP_MANAGER 3u

/*
 * OP_NET_CONN_STATE (0xB9) — query lwIP connection state for a vNIC slot
 *   MR1 = vnic_id
 *   Reply:
 *   MR0 = result
 *   MR1 = state  (0=free, 1=connecting, 2=connected, 3=closed)
 */
#define OP_NET_CONN_STATE       0xB9u

/*
 * OP_NET_TCP_CLOSE (0xBA) — close the lwIP TCP connection for a vNIC slot
 *   MR1 = vnic_id
 *   Reply:
 *   MR0 = result
 */
#define OP_NET_TCP_CLOSE        0xBAu

/* ── IPC Opcodes ─────────────────────────────────────────────────────────── */

/*
 * OP_NET_VNIC_CREATE (0xB0) — create a new virtual NIC
 *   MR1 = requested_vnic_id  (0xFF = auto-assign)
 *   MR2 = cap_classes        (must include CAP_CLASS_NET, else NET_ERR_PERM)
 *   MR3 = caller_pd_id       (recorded as owner for ACL)
 *   Reply:
 *   MR0 = result (NET_OK or NET_ERR_*)
 *   MR1 = assigned_vnic_id
 *   MR2 = shmem_slot_offset  (byte offset into net_packet_shmem for this vNIC)
 */
#define OP_NET_VNIC_CREATE      0xB0u

/*
 * OP_NET_VNIC_DESTROY (0xB1) — destroy a vNIC and free its slot
 *   MR1 = vnic_id
 *   Reply:
 *   MR0 = result
 */
#define OP_NET_VNIC_DESTROY     0xB1u

/*
 * OP_NET_VNIC_SEND (0xB2) — transmit a packet from a vNIC
 *   MR1 = vnic_id
 *   MR2 = packet_offset  (byte offset into net_packet_shmem)
 *   MR3 = packet_len     (bytes)
 *   Reply:
 *   MR0 = result
 *   MR1 = bytes_sent
 *
 *   MVP: copies packet to virtio-net TX queue stub; logs metadata.
 *        Loopback (127.x.x.x dest) is routed to the appropriate local vNIC.
 *        ACL is enforced: NET_ACL_ALLOW_OUTBOUND must be set.
 *   SMOLTCP_INTEGRATION_POINT: feed packet to smoltcp interface.
 */
#define OP_NET_VNIC_SEND        0xB2u

/*
 * OP_NET_VNIC_RECV (0xB3) — receive a packet into a buffer
 *   MR1 = vnic_id
 *   MR2 = buf_offset  (byte offset into net_packet_shmem)
 *   MR3 = buf_len     (max bytes to receive)
 *   Reply:
 *   MR0 = result
 *   MR1 = bytes_received  (0 in MVP — no real RX yet)
 *
 *   SMOLTCP_INTEGRATION_POINT: poll smoltcp for received packets.
 */
#define OP_NET_VNIC_RECV        0xB3u

/*
 * OP_NET_BIND (0xB4) — bind a port to a vNIC
 *   MR1 = vnic_id
 *   MR2 = port      (1..65535)
 *   MR3 = protocol  (NET_PROTO_TCP=0, NET_PROTO_UDP=1)
 *   Reply:
 *   MR0 = result
 */
#define OP_NET_BIND             0xB4u

/*
 * OP_NET_CONNECT (0xB5) — connect a vNIC to a remote endpoint
 *   MR1 = vnic_id
 *   MR2 = dest_ip    (packed u32, big-endian)
 *   MR3 = dest_port
 *   MR4 = protocol   (NET_PROTO_TCP=0, NET_PROTO_UDP=1)
 *   Reply:
 *   MR0 = NET_ERR_STUB (not yet implemented — smoltcp integration pending)
 *   MR1 = conn_id     (0 in stub)
 *
 *   SMOLTCP_INTEGRATION_POINT: open smoltcp TCP/UDP socket.
 */
#define OP_NET_CONNECT          0xB5u

/*
 * OP_NET_STATUS (0xB6) — query vNIC or global status
 *   MR1 = vnic_id  (0xFFFFFFFF = global status)
 *   Reply:
 *   MR0 = result
 *   MR1 = active_vnics
 *   MR2 = rx_packets  (for specific vnic; global total for 0xFFFFFFFF)
 *   MR3 = tx_packets
 */
#define OP_NET_STATUS           0xB6u

/*
 * OP_NET_SET_ACL (0xB7) — update ACL flags for a vNIC
 *   MR1 = vnic_id
 *   MR2 = acl_flags  (NET_ACL_* bitmask)
 *   Reply:
 *   MR0 = result
 */
#define OP_NET_SET_ACL          0xB7u

/*
 * OP_NET_HEALTH (0xB8) — liveness check
 *   Reply:
 *   MR0 = NET_OK (0)
 *   MR1 = active_vnic_count
 *   MR2 = NET_VERSION
 */
#define OP_NET_HEALTH           0xB8u

/*
 * OP_NET_HTTP_POST (0x500) — high-level HTTP POST proxy to the agentOS bridge.
 *
 * Used by libagent's aos_http_post() to reach the host-side bridge
 * at 10.0.2.2:8790 (QEMU user-networking host address) without
 * requiring agents to manage raw TCP sockets themselves.
 *
 * Inputs (MRs set by caller):
 *   MR1 = staging offset of the URL string       (relative to vibe_staging base)
 *   MR2 = URL length (bytes, not including NUL)
 *   MR3 = staging offset of the request body     (relative to vibe_staging base)
 *   MR4 = body length (bytes)
 *
 * The URL and body must be placed in the vibe_staging shared region before
 * calling. net_server maps vibe_staging via setvar_vaddr (same physical pages
 * as the VibeEngine's vibe_staging_vaddr).
 *
 * Outputs (MRs written by net_server):
 *   MR0 = HTTP status code (e.g. 200, 422, 503), or 0 on TCP error
 *   MR1 = staging offset of the response body
 *   MR2 = response body length
 *
 * Note: only one HTTP proxy connection at a time (uses reserved conn slot
 * HTTP_CONN_ID = NET_MAX_VNICS - 1 in g_conns[]).
 */
#define OP_NET_HTTP_POST        0x500u

/* ── Result codes (MR0 in replies) ──────────────────────────────────────── */
#define NET_OK                  0u   /* success */
#define NET_ERR_NO_VNICS        1u   /* vNIC table full */
#define NET_ERR_NOT_FOUND       2u   /* vnic_id not registered */
#define NET_ERR_PERM            3u   /* capability or ACL denied */
#define NET_ERR_STUB            4u   /* feature not yet implemented */
#define NET_ERR_INVAL           5u   /* invalid argument */

/* ── virtio-net MMIO constants ───────────────────────────────────────────── */
/*
 * NetServer probes the virtio-net MMIO region during init().
 * The MMIO base virtual address is provided via Microkit setvar_vaddr.
 * See virtio_blk.h for the common VIRTIO_MMIO_* register offset definitions.
 * virtio device_id 1 = network card.
 */
#define VIRTIO_NET_DEVICE_ID    1u   /* network card */

/* virtio-net config space: capacity field offset (bytes from MMIO_CONFIG) */
#define VIRTIO_NET_CONFIG_MAC   0x00u  /* 6-byte MAC address */
#define VIRTIO_NET_CONFIG_STATUS 0x06u /* link status (uint16_t) */

/* virtio-net status bits */
#define VIRTIO_NET_S_LINK_UP    (1u << 0)

/* ── vNIC table entry ────────────────────────────────────────────────────── */
/*
 * Stored statically in NetServer's BSS.  Not visible to callers;
 * exposed here for documentation and test-harness access.
 */
typedef struct {
    bool      active;
    uint32_t  vnic_id;
    uint32_t  owner_pd;                    /* PD that created this vNIC */
    uint32_t  cap_classes;                 /* CAP_CLASS_* bitmask at creation */
    uint32_t  acl_flags;                   /* NET_ACL_* bitmask */
    uint32_t  bound_ports[NET_MAX_BOUND_PORTS];
    uint32_t  port_count;
    uint32_t  shmem_slot;                  /* index 0..15 into net_packet_shmem */
    uint8_t   mac[6];                      /* 02:00:00:00:00:NN */
    uint8_t   _mac_pad[2];
    uint64_t  rx_packets;
    uint64_t  tx_packets;
} net_vnic_t;                              /* ~72 bytes */

/* ── shmem slot offset helper ────────────────────────────────────────────── */
/*
 * Returns the byte offset of slot N's ring header within net_packet_shmem.
 * Add NET_SHMEM_HDR_SIZE to get the start of packet data.
 */
#define NET_SLOT_OFFSET(n)  ((n) * NET_SHMEM_SLOT_SIZE)

/* ── Loopback detection ──────────────────────────────────────────────────── */
/*
 * A destination IPv4 address is loopback if its top byte is 0x7F (127.x.x.x).
 * Addresses are packed u32, big-endian (network byte order).
 */
#define NET_IP_IS_LOOPBACK(ip)  (((ip) >> 24) == 0x7Fu)
