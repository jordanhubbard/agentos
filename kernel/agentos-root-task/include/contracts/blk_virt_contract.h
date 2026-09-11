/*
 * contracts/blk_virt_contract.h — blk_virt (block virtualizer PD) contract
 *
 * // STATUS: IMPLEMENTED
 *
 * blk_virt is the only block mux (docs/TCB.md, I/O invariant 2).  It owns no
 * device frame and no IRQ.  Requests and responses move through the
 * sDDF-shaped guest queues in the shared block region
 * (platform/include/platform/blk_layout.h, mapped at AOS_BLK_SHMEM_VA in
 * every guest VMM and in blk_virt); this contract carries only control
 * (attach) and notifications (kicks).  There is no per-request IPC between a
 * VMM and blk_virt, and no VMM holds the virtio_blk driver endpoint or the
 * driver's DMA window.
 *
 * Transport:
 *   - ATTACH is a seL4 Call on blk_virt's listen endpoint.  The request and
 *     reply payloads are the packed structs below, carried in
 *     sel4_msg_t.data (opcode in sel4_msg_t.opcode / the message label).
 *     On success blk_virt has filled the client's storage_info page (sDDF
 *     blk_storage_info_t at AOS_BLK_STORAGE_INFO_OFF) from the host media,
 *     or from its RAM disk when the driver reports no media.
 *   - KICK is a seL4_NBSend from a VMM to blk_virt with label
 *     BLK_VIRT_EVENT_KICK and no payload: "my request queue is non-empty".
 *     The sender never blocks; a kick that lands while blk_virt is not in
 *     Recv is dropped, which is why the req_consumer_signalled word below
 *     makes every kick re-sendable and blk_virt rescans before blocking.
 *   - RESP_READY is a seL4_NBSend from blk_virt to the owning VMM's listen
 *     endpoint with label BLK_VIRT_EVENT_RESP_READY: "your response queue is
 *     non-empty".  A VMM also drains its response queue after every guest
 *     exit, so a dropped RESP_READY costs latency, never a response.
 *
 * Signalling protocol (per client, aos_blk_signal_t at AOS_BLK_SIGNAL_OFF):
 *   req_consumer_signalled  consumer = blk_virt.  blk_virt sets it to 1
 *                           while draining and to 0 before it blocks.  A VMM
 *                           kicks when its request queue is non-empty and
 *                           the word is 0.  The VMM never writes the word, so
 *                           a lost kick is repeated on the guest's next exit.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

#define BLK_VIRT_CONTRACT_VERSION       1u

/* ── Opcodes / labels ─────────────────────────────────────────────────── */

/* Call, VMM -> blk_virt: bind guest client `client_id` (queue stride) to
 * host media `media_id`; `vmm_slot` names the caller so blk_virt knows which
 * listen EP receives RESP_READY. */
#define BLK_VIRT_OP_ATTACH              0x2C01u
/* NBSend, VMM -> blk_virt: request queue is non-empty. */
#define BLK_VIRT_EVENT_KICK             0x2C10u
/* NBSend, blk_virt -> VMM: response queue is non-empty. */
#define BLK_VIRT_EVENT_RESP_READY       0x2C11u

/* ── VMM slots (blk_virt_attach_req_t.vmm_slot) ───────────────────────── */
#define BLK_VIRT_VMM_SLOT_PRIMARY       0u
#define BLK_VIRT_VMM_SLOT_SECONDARY     1u

/* ── Status codes (blk_virt_attach_reply_t.status) ────────────────────── */
#define BLK_VIRT_OK                     0u
#define BLK_VIRT_ERR_VERSION            1u   /* request version unsupported */
#define BLK_VIRT_ERR_BAD_CLIENT         2u   /* client/vmm_slot/media out of range */
#define BLK_VIRT_ERR_BUSY               3u   /* client already attached */
#define BLK_VIRT_ERR_UNAVAILABLE        4u   /* virtualizer not serving yet */

/* ── Backend reported by ATTACH (blk_virt_attach_reply_t.hw_state) ────── */
#define BLK_VIRT_HW_NONE                0u   /* no host media: RAM disk backend */
#define BLK_VIRT_HW_VIRTIO_BLK          1u   /* requests reach virtio_blk's media */

typedef struct __attribute__((packed)) {
    uint32_t version;    /* BLK_VIRT_CONTRACT_VERSION */
    uint32_t client_id;  /* 0 .. AOS_BLK_MAX_CLIENTS-1 (queue stride index) */
    uint32_t vmm_slot;   /* BLK_VIRT_VMM_SLOT_* */
    uint32_t media_id;   /* AOS_HOST_BLK_MEDIA_* (profile block_media) */
} blk_virt_attach_req_t;

typedef struct __attribute__((packed)) {
    uint32_t status;     /* BLK_VIRT_OK / BLK_VIRT_ERR_* */
    uint32_t version;    /* BLK_VIRT_CONTRACT_VERSION served by blk_virt */
    uint32_t hw_state;   /* BLK_VIRT_HW_* */
    uint32_t _pad;
    uint64_t capacity;   /* guest-visible capacity in AOS_BLK_TRANSFER_SIZE units */
} blk_virt_attach_reply_t;
