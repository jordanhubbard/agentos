/*
 * contracts/net_virt_contract.h — net_virt (network virtualizer PD) contract
 *
 * // STATUS: IMPLEMENTED
 *
 * net_virt is the only network mux (docs/TCB.md, I/O invariant 2).  It owns
 * no device frame and no IRQ.  Frames move through the sDDF-shaped guest
 * queues in separate client pages (platform/include/platform/net_layout.h).
 * Each VMM maps its own page, net_pd maps a separate transfer page, and
 * net_virt maps the complete region at AOS_NET_SHMEM_VA. This contract
 * carries only control (attach) and notifications (kicks).  There is no
 * per-frame IPC between a VMM and net_virt.
 *
 * Transport:
 *   - ATTACH is a seL4 Call on net_virt's listen endpoint.  The request and
 *     reply payloads are the packed structs below, carried in
 *     sel4_msg_t.data (opcode in sel4_msg_t.opcode / the message label).
 *   - KICK signals net_virt's bound notification using a root-minted
 *     send-only cap, badged 1 or 2 for the guest client. Signals remain
 *     pending until received and may combine with the native-client badge.
 *   - net_virt signals the owning VMM's bound notification with
 *     NET_VIRT_VMM_WAKE_BADGE when RX active has frames. Notification badges
 *     take precedence over IPC message tags, which may be stale.
 *
 * Signalling protocol (per client, over the sDDF queue flags):
 *   tx_active.consumer_signalled  consumer = net_virt.  net_virt sets it to
 *                                 1 while draining and to 0 before it
 *                                 blocks.  A VMM kicks when tx_active is
 *                                 non-empty and the flag is 0.  The VMM never
 *                                 sets the flag; redundant signals coalesce.
 *   rx_free.consumer_signalled    consumer = net_virt.  1 normally; net_virt
 *                                 clears it when it stopped pulling RX
 *                                 because the guest RX free queue was empty.
 *                                 A VMM kicks after recycling RX buffers
 *                                 when the flag is 0.
 *   rx_active.consumer_signalled  consumer = VMM (libvmm manages it).
 *                                 net_virt does not suppress RX_READY on it.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

/* Version 3 isolates queue clients and driver transfers on separate pages.
 * Version 2 requires root-minted virtualizer_authority.h badges. */
#define NET_VIRT_CONTRACT_VERSION       6u
/* Version 4 adds an isolated native client page before the driver page.
 * The native lane uses root-provisioned persistent notifications, not
 * dropped endpoint events. A wake badge takes precedence over message info. */
#define NET_VIRT_NATIVE_WAKE_BADGE UINT64_C(0x40000000)
/* Version 5 extends persistent wakeups to guest clients. */
#define NET_VIRT_GUEST_WAKE_MASK UINT64_C(3)
#define NET_VIRT_VMM_WAKE_BADGE (UINT64_C(1) << 59)

/* ── Opcodes / labels ─────────────────────────────────────────────────── */

/* Call, VMM -> net_virt: bind guest client `client_id` to the virtualizer.
 * `vmm_slot` selects its root-granted notification; `client_id` selects the
 * queue stride (profile network_client). */
#define NET_VIRT_OP_ATTACH              0x2201u
/* Version 6: terminal queue detach, with the same request/reply layout and
 * badge authorization as ATTACH. Stop the producer before calling. An OK
 * reply guarantees that net_virt holds no queue pointers for this client;
 * queued packets may be discarded. Repeated detach is idempotent. This does
 * close the raw driver handle before retiring the queue. Reattachment uses
 * REBIND; legacy ATTACH cannot revive a retired client. */
#define NET_VIRT_OP_DETACH              0x2202u
/* Guest-only control: one private untyped capability in, one fresh queue
 * frame capability out on success. The VMM retains pool revocation authority.
 * Client N implies VMM slot N. Only the next nonzero generation is accepted.
 * On failure revoke the pool before retrying; detach a committed generation
 * first if receiving or mapping the returned frame fails. */
#define NET_VIRT_OP_REBIND              0x2203u
#define NET_VIRT_REBIND_VERSION         1u
typedef struct __attribute__((packed)) {
    uint32_t version, client, generation;
} net_virt_rebind_req_t;
typedef struct __attribute__((packed)) {
    uint32_t status, version, generation;
} net_virt_rebind_reply_t;
/* NBSend, VMM -> net_virt: guest queues changed (see header comment). */
#define NET_VIRT_EVENT_KICK             0x2210u

/* ── Status codes (net_virt_attach_reply_t.status) ────────────────────── */
#define NET_VIRT_OK                     0u
#define NET_VIRT_ERR_VERSION            1u   /* request version unsupported */
#define NET_VIRT_ERR_BAD_CLIENT         2u   /* client_id out of range / no EP */
#define NET_VIRT_ERR_BUSY               3u   /* client already attached */
#define NET_VIRT_ERR_UNAVAILABLE        4u   /* virtualizer not bridging yet */
#define NET_VIRT_ERR_RESOURCE           5u

/* ── Hardware state reported by ATTACH (net_virt_attach_reply_t.hw_state) */
#define NET_VIRT_HW_NONE                0u   /* no host NIC: hub/loopback pump */
#define NET_VIRT_HW_NET_PD              1u   /* frames reach net_pd's host NIC */

/* net_virt_attach_req_t.vmm_slot */
#define NET_VIRT_VMM_SLOT_PRIMARY       0u
#define NET_VIRT_VMM_SLOT_SECONDARY     1u
#define NET_VIRT_SLOT_NATIVE            2u

typedef struct __attribute__((packed)) {
    uint32_t version;    /* NET_VIRT_CONTRACT_VERSION */
    uint32_t client_id;  /* 0 .. AOS_NET_QUEUE_CLIENTS-1 (queue stride index) */
    uint32_t vmm_slot;   /* root assignment: VMM slot 0/1 or native slot 2 */
} net_virt_attach_req_t;

typedef struct __attribute__((packed)) {
    uint32_t status;     /* NET_VIRT_OK / NET_VIRT_ERR_* */
    uint32_t version;    /* NET_VIRT_CONTRACT_VERSION served by net_virt */
    uint32_t hw_state;   /* NET_VIRT_HW_* */
    uint8_t  mac[6];     /* MAC net_pd assigned to this client (hw only) */
    uint8_t  _pad[2];
} net_virt_attach_reply_t;
