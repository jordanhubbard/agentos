/*
 * contracts/blk_virt_contract.h — blk_virt (block virtualizer PD) contract
 *
 * // STATUS: IMPLEMENTED
 *
 * blk_virt is the only block mux (docs/TCB.md, I/O invariant 2).  It owns no
 * device frame and no IRQ.  Requests and responses move through the
 * sDDF-shaped guest queues in the shared block region
 * (platform/include/platform/blk_layout.h; blk_virt maps the whole region,
 * each guest VMM maps only its client page); this contract carries only control
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
 *   - KICK is a seL4_Signal on a send-only capability to blk_virt's bound
 *     notification, badged with the client's bit (0 or 1).
 *   - RESP_READY is a seL4_Signal to the owning VMM's bound notification,
 *     badged BLK_VIRT_VMM_WAKE_BADGE. Signals remain pending until received;
 *     the receiver scans queues after waking. This also works before the
 *     guest starts, when no guest exit can retry a dropped endpoint event.
 *
 * Signalling protocol (per client, aos_blk_signal_t at AOS_BLK_SIGNAL_OFF):
 *   req_consumer_signalled  consumer = blk_virt.  blk_virt sets it to 1
 *                           while draining and to 0 before it blocks.  A VMM
 *                           kicks when its request queue is non-empty and
 *                           the word is 0.  The VMM never writes the word, so
 *                           notification delivery does not depend on guest exits.
 *
 * Queue request ABI: version 4 exposes only READ, WRITE, FLUSH and BARRIER
 * (the sDDF values in aos_blk_req_code_t).  It does not negotiate DISCARD.
 * Any unrecognised request code, including a discard-shaped code, completes
 * with BLK_RESP_ERR_INVALID_PARAM and is never sent to the backend.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

/* Version 4 uses persistent queue notifications in both directions.
 * Version 3 binds client/media assignment to virtualizer_authority.h badges;
 * version 2 introduced separately mapped large-page client strides. */
#define BLK_VIRT_CONTRACT_VERSION       5u

/* Persistent notification caps; only queue ownership conveys data authority.
 * Client bits coalesce at the virtualizer, which scans all attached queues.
 * VMMs classify this bit before interpreting stale IPC message registers. */
#define BLK_VIRT_VMM_WAKE_BADGE (UINT64_C(1) << 60)
static inline int blk_virt_service_notification(uint64_t badge)
{
    return badge != 0 && (badge & ~UINT64_C(3)) == 0;
}

/* ── Opcodes / labels ─────────────────────────────────────────────────── */

/* Call, VMM -> blk_virt: bind guest client `client_id` (queue stride) to
 * host media `media_id`; `vmm_slot` names the caller so blk_virt knows which
 * notification receives RESP_READY. */
#define BLK_VIRT_OP_ATTACH              0x2C01u
/* Version 5 terminal detach uses the attach request/reply layouts and the
 * same root-assigned client/slot/media authority. The producer must stop
 * admission and consume all responses first. BUSY preserves the attachment
 * while either queue is nonempty or invalid. OK retires all service queue
 * references. Repeated detach is idempotent; retired clients must use REBIND
 * with a fresh pool and generation instead of ATTACH. This is not a flush. */
#define BLK_VIRT_OP_DETACH              0x2C02u
/* A retired guest supplies one private untyped capability. The service
 * retypes one large queue frame and returns it after binding the same media.
 * The caller retains the pool and must revoke it after detach, or after an
 * unsuccessful reconstruction. ATTACH cannot revive a retired client.
 * Client N implies VMM slot/media N. Generation starts at 1 and increments
 * once per successful rebind; exhaustion fails closed. */
#define BLK_VIRT_OP_REBIND              0x2C03u
#define BLK_VIRT_REBIND_VERSION         2u
/* Legacy endpoint labels retained for diagnostics/compatibility; version 4
 * clients use the notification capabilities, not these labels. */
/* VMM -> blk_virt: request queue is non-empty. */
#define BLK_VIRT_EVENT_KICK             0x2C10u
/* blk_virt -> VMM: response queue is non-empty. */
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
#define BLK_VIRT_ERR_RESOURCE           5u

typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t client;
    uint32_t generation;
} blk_virt_rebind_req_t;

/* One frame capability accompanies successful replies only. Storage info
 * in the new frame carries backend geometry and write policy. Version 2
 * also returns the current backend kind for fresh VMM device adoption. */
typedef struct __attribute__((packed)) {
    uint32_t status;
    uint32_t version;
    uint32_t generation;
    uint32_t hw_state;
} blk_virt_rebind_reply_t;

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
