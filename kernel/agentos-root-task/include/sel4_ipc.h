/*
 * sel4_ipc.h — seL4 IPC message abstractions for agentOS service PDs
 *
 * Provides the canonical sel4_msg_t / sel4_badge_t types and the thin
 * inline wrappers (sel4_call, sel4_reply, sel4_recv, sel4_reply_recv)
 * that all agentOS service PDs use.  Built on top of Microkit MR primitives.
 *
 * Design notes:
 *   - sel4_msg_t packs the first 48 bytes of payload into inline data[].
 *     This covers the most common small-message case without allocation.
 *   - All fields are explicitly sized (uint32_t / uint8_t) so the layout
 *     is identical on 32-bit and 64-bit targets.
 *   - No libc dependency.  No dynamic allocation.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

/*
 * In production (seL4 bare-metal) builds include the real seL4 types.
 * In AGENTOS_TEST_HOST builds the framework.h mock layer provides stubs;
 * define the minimal types needed so this header compiles standalone.
 */
#ifndef AGENTOS_TEST_HOST
#  include <sel4/sel4.h>  /* seL4_CPtr / seL4_Word / seL4_SetMR / seL4_GetMR */
/* Shim: map microkit_mr_set/get to seL4_SetMR/seL4_GetMR for legacy callers */
#  ifndef microkit_mr_set
#    define microkit_mr_set(i, v) seL4_SetMR((int)(i), (seL4_Word)(v))
#  endif
#  ifndef microkit_mr_get
#    define microkit_mr_get(i)    seL4_GetMR((int)(i))
#  endif
#else
/* framework.h provides microkit_mr_set / microkit_mr_get stubs */
typedef uint64_t seL4_CPtr;
typedef uint64_t seL4_Word;
#endif

/* ── Error / status codes ────────────────────────────────────────────────── */

#define SEL4_ERR_OK           0u   /* success                              */
#define SEL4_ERR_INVALID_OP   1u   /* unknown opcode                       */
#define SEL4_ERR_NOT_FOUND    2u   /* requested resource does not exist    */
#define SEL4_ERR_PERM         3u   /* caller lacks required capability     */
#define SEL4_ERR_BAD_ARG      4u   /* malformed request                    */
#define SEL4_ERR_NO_MEM       5u   /* server-side allocation failure       */
#define SEL4_ERR_BUSY         6u   /* resource temporarily unavailable     */
#define SEL4_ERR_OVERFLOW     7u   /* client-side buffer too small         */
#define SEL4_ERR_INTERNAL     8u   /* unrecoverable server error           */
#define SEL4_ERR_FORBIDDEN    9u   /* capability policy rejects the caller */

/* ── Message geometry ────────────────────────────────────────────────────── */

/*
 * SEL4_MSG_DATA_BYTES: number of inline payload bytes in sel4_msg_t.data[].
 *
 * 48 bytes fits comfortably within Microkit's 64-MR limit:
 *   MR0  = opcode  (uint32, occupies one seL4_Word)
 *   MR1  = length  (uint32, occupies one seL4_Word)
 *   MR2..MR7 = 6 × 8-byte seL4_Words = 48 bytes of data
 *
 * Total MR count: 8, well within the 64-MR cap.
 */
#define SEL4_MSG_DATA_BYTES  48u

/* ── Core types ─────────────────────────────────────────────────────────── */

/*
 * sel4_msg_t — a fixed-size IPC message.
 *
 * Used for both requests and replies.  The receiver always gets a fresh
 * copy; no aliasing between caller and callee.
 *
 * Fields:
 *   opcode  — operation selector on request; status code on reply
 *   length  — valid bytes in data[]  (0 .. SEL4_MSG_DATA_BYTES)
 *   data[]  — inline payload bytes
 */
typedef struct {
    uint32_t opcode;
    uint32_t length;
    uint8_t  data[SEL4_MSG_DATA_BYTES];
} sel4_msg_t;

_Static_assert(sizeof(sel4_msg_t) == 4u + 4u + SEL4_MSG_DATA_BYTES,
               "sel4_msg_t layout mismatch");

/*
 * sel4_badge_t — capability badge delivered with an incoming IPC.
 *
 * On seL4 the badge is a seL4_Word embedded in the badged endpoint cap.
 * We expose it as a typed uint64_t so 64-bit badge values are legal on
 * both 32-bit and 64-bit targets (truncation on 32-bit is intentional and
 * documented in the seL4 reference manual §4.2.1).
 */
typedef uint64_t sel4_badge_t;

/* ── MR layout helpers (internal) ─────────────────────────────────────────
 *
 *   MR0: opcode / status  (lower 32 bits of seL4_Word)
 *   MR1: length           (lower 32 bits of seL4_Word)
 *   MR2..MR7: data[0..47] packed as 6 × 8-byte words (little-endian)
 *
 * On a 64-bit target each seL4_Word = 8 bytes, so MR2 holds data[0..7],
 * MR3 holds data[8..15], … MR7 holds data[40..47].
 * On a 32-bit target each seL4_Word = 4 bytes, so MR2..MR13 are used;
 * the helpers pack/unpack accordingly by walking sizeof(seL4_Word) at a
 * time.
 */

#define _SEL4_MR_DATA_START  2u          /* first MR carrying payload bytes */
#define _SEL4_MR_DATA_COUNT  (SEL4_MSG_DATA_BYTES / sizeof(seL4_Word))

/* Pack sel4_msg_t → MRs */
static inline void _sel4_msg_to_mrs(const sel4_msg_t *msg)
{
    microkit_mr_set(0, (seL4_Word)msg->opcode);
    microkit_mr_set(1, (seL4_Word)msg->length);
    for (uint32_t w = 0; w < _SEL4_MR_DATA_COUNT; w++) {
        seL4_Word word = 0;
        const uint8_t *src = msg->data + w * sizeof(seL4_Word);
        for (uint32_t b = 0; b < sizeof(seL4_Word); b++) {
            word |= ((seL4_Word)src[b]) << (b * 8u);
        }
        microkit_mr_set(_SEL4_MR_DATA_START + w, word);
    }
}

/* Unpack MRs → sel4_msg_t */
static inline void _sel4_mrs_to_msg(sel4_msg_t *msg)
{
    msg->opcode = (uint32_t)microkit_mr_get(0);
    msg->length = (uint32_t)microkit_mr_get(1);
    if (msg->length > SEL4_MSG_DATA_BYTES)
        msg->length = SEL4_MSG_DATA_BYTES;
    for (uint32_t w = 0; w < _SEL4_MR_DATA_COUNT; w++) {
        seL4_Word word = microkit_mr_get(_SEL4_MR_DATA_START + w);
        uint8_t *dst = msg->data + w * sizeof(seL4_Word);
        for (uint32_t b = 0; b < sizeof(seL4_Word); b++) {
            dst[b] = (uint8_t)((word >> (b * 8u)) & 0xFFu);
        }
    }
}

/* Number of MRs to declare in msginfo (opcode + length + data words) */
#define _SEL4_MR_COUNT  (_SEL4_MR_DATA_START + _SEL4_MR_DATA_COUNT)

/* ── IPC primitives ──────────────────────────────────────────────────────── */

/*
 * sel4_call() — send request on ep, block until reply.
 *
 * Equivalent to seL4_Call.  Fills *rep with the server's reply.
 * Badge of the reply endpoint is not observable to the client (seL4 zeroes
 * it for Call); callers that need badge must use sel4_recv instead.
 */
static inline void sel4_call(seL4_CPtr ep,
                              const sel4_msg_t *req,
                              sel4_msg_t *rep)
{
    _sel4_msg_to_mrs(req);
#ifndef AGENTOS_TEST_HOST
    seL4_MessageInfo_t _info = seL4_MessageInfo_new(
        (seL4_Word)req->opcode, 0, 0, (seL4_Word)_SEL4_MR_COUNT);
    (void)seL4_Call(ep, _info);
#endif
    _sel4_mrs_to_msg(rep);
}

/*
 * sel4_reply() — send a reply to the current caller (no blocking).
 *
 * Must only be called from within a server's protected procedure call
 * (PPC) handler.  Uses the implicit reply capability held by the PD.
 * Equivalent to seL4_Reply.
 *
 * NOTE: Microkit does not expose a direct seL4_Reply wrapper in all
 * versions.  The standard Microkit pattern is to call microkit_ppcall to
 * reply + recv atomically (sel4_reply_recv below).  A standalone reply is
 * modelled here via sel4_reply_recv with a sentinel that the caller must
 * never actually block on — but since seL4 Microkit manages the reply cap
 * lifetime this is only used as a logical annotation.  In practice servers
 * should use sel4_reply_recv exclusively.  This function is provided for
 * callers that need the conceptual separation.
 */
static inline void sel4_reply(const sel4_msg_t *rep)
{
    /* Load the reply into MRs; the actual seL4_Reply will be triggered by
     * the next sel4_reply_recv call.  For standalone-reply semantics in a
     * non-Microkit build, replace this body with seL4_Reply(). */
    _sel4_msg_to_mrs(rep);
}

/*
 * sel4_recv() — block on ep until a message arrives.
 *
 * Returns the badge of the sender's endpoint capability.
 * Fills *msg with the received message.
 * Equivalent to seL4_Recv.
 *
 * First-call bootstrap: used by sel4_server_run before any reply is pending.
 */
static inline sel4_badge_t sel4_recv(seL4_CPtr ep, sel4_msg_t *msg)
{
    sel4_badge_t badge = 0;
#ifndef AGENTOS_TEST_HOST
    seL4_Word _badge = 0;
    (void)seL4_Recv(ep, &_badge);
    badge = (sel4_badge_t)_badge;
#else
    (void)ep;
#endif
    _sel4_mrs_to_msg(msg);
    return badge;
}

/*
 * sel4_reply_recv() — atomically reply to current caller and wait for next.
 *
 * This is the standard server loop primitive (seL4_ReplyRecv).
 * Sends *rep as reply, then blocks on ep until next message arrives.
 * Returns the badge of the new sender; fills *req with new request.
 *
 * In Microkit servers this maps to: finish handling the current PPC,
 * send the reply via msginfo, and immediately block for the next PPC.
 */
static inline sel4_badge_t sel4_reply_recv(seL4_CPtr ep,
                                            const sel4_msg_t *rep,
                                            sel4_msg_t *req)
{
    sel4_badge_t badge = 0;
    _sel4_msg_to_mrs(rep);
#ifndef AGENTOS_TEST_HOST
    seL4_MessageInfo_t _info = seL4_MessageInfo_new(
        (seL4_Word)rep->opcode, 0, 0, (seL4_Word)_SEL4_MR_COUNT);
    seL4_Word _badge = 0;
    (void)seL4_ReplyRecv(ep, _info, &_badge);
    badge = (sel4_badge_t)_badge;
#endif
    _sel4_mrs_to_msg(req);
    return badge;
}
