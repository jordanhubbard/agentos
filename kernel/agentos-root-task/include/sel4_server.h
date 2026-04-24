/*
 * sel4_server.h — standard server-side recv/dispatch/reply loop
 *
 * Provides the canonical server dispatch pattern used by all agentOS
 * service protection domains.  A PD:
 *   1. Calls sel4_server_init() once with its listen endpoint.
 *   2. Calls sel4_server_register() for each opcode it handles.
 *   3. Calls sel4_server_run(), which never returns.
 *
 * All functions are static inline — no separate compilation unit needed.
 * Include this header, link against nothing extra.
 *
 * Example:
 *
 *   static uint32_t my_ping(sel4_badge_t b, const sel4_msg_t *req,
 *                            sel4_msg_t *rep, void *ctx) {
 *       (void)b; (void)req; (void)ctx;
 *       rep->length = 0;
 *       return SEL4_ERR_OK;
 *   }
 *
 *   void pd_init(void) {
 *       static sel4_server_t srv;
 *       sel4_server_init(&srv, MY_ENDPOINT_CAP);
 *       sel4_server_register(&srv, OP_PING, my_ping, NULL);
 *       sel4_server_run(&srv);
 *   }
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "sel4_ipc.h"

/* ── Capacity ──────────────────────────────────────────────────────────── */

/* Maximum number of opcode handlers one server can register. */
#define SEL4_SERVER_MAX_HANDLERS  32u

/*
 * SEL4_SERVER_OPCODE_ANY — wildcard opcode sentinel.
 *
 * When registered with sel4_server_register(), this handler is invoked as a
 * fallback when no exact opcode match is found in the handler table.
 * Only one ANY handler is consulted; the first registered ANY wins.
 *
 * Use this to implement PDs whose existing protected() switch already reads
 * and dispatches on msg_u32(req, 0) internally — register one ANY handler
 * wrapping the full switch, rather than splitting every opcode into a
 * separate registered handler.
 */
#define SEL4_SERVER_OPCODE_ANY  0xFFFFFFFFu

/* ── Handler function type ───────────────────────────────────────────────
 *
 * sel4_handler_fn — called by sel4_server_dispatch for a matching opcode.
 *
 * Parameters:
 *   badge  — capability badge of the sender (carries identity / policy)
 *   req    — incoming request message (read-only)
 *   rep    — reply message to populate before returning
 *   ctx    — opaque context pointer registered with sel4_server_register
 *
 * Return value:
 *   SEL4_ERR_OK (0) on success, or any SEL4_ERR_* code on failure.
 *   The return value is also placed in rep->opcode by the caller so the
 *   client always sees the status in MR0 on reply.
 */
typedef uint32_t (*sel4_handler_fn)(sel4_badge_t badge,
                                     const sel4_msg_t *req,
                                     sel4_msg_t *rep,
                                     void *ctx);

/* ── Server state ────────────────────────────────────────────────────────
 *
 * sel4_server_t — one instance per service PD.
 *
 * Typically declared as a static or global variable in the PD's main
 * compilation unit; stack allocation is also safe provided the server loop
 * is entered from the same frame (it never returns).
 */
typedef struct {
    struct {
        uint32_t        opcode;
        sel4_handler_fn fn;
        void           *ctx;
    } handlers[SEL4_SERVER_MAX_HANDLERS];
    uint32_t  handler_count;
    seL4_CPtr ep;             /* listen endpoint capability */
} sel4_server_t;

/* Verify the struct fits in one page — a server state that overflows a page
 * would be dangerous on architectures where the kernel maps only one page
 * for the initial thread stack. */
_Static_assert(sizeof(sel4_server_t) < 4096u,
               "sel4_server_t fits in a page");

/* ── Initialisation ──────────────────────────────────────────────────────
 *
 * sel4_server_init() — zero handler table and record listen endpoint.
 *
 * Must be called before any sel4_server_register() or sel4_server_run().
 *
 * ep: the seL4 endpoint capability on which the server will receive RPCs.
 */
static inline void sel4_server_init(sel4_server_t *srv, seL4_CPtr ep)
{
    srv->handler_count = 0;
    srv->ep            = ep;
    /* Zero handler table explicitly for -nostdinc builds (no memset). */
    for (uint32_t i = 0; i < SEL4_SERVER_MAX_HANDLERS; i++) {
        srv->handlers[i].opcode = 0;
        srv->handlers[i].fn     = (sel4_handler_fn)0;
        srv->handlers[i].ctx    = (void *)0;
    }
}

/* ── Handler registration ────────────────────────────────────────────────
 *
 * sel4_server_register() — bind a handler function to an opcode.
 *
 * Opcodes are matched in registration order; the first match wins.
 * Duplicate opcode registrations are legal (only the first will fire).
 *
 * Returns:
 *    0   success
 *   -1   handler table full (SEL4_SERVER_MAX_HANDLERS reached)
 */
static inline int sel4_server_register(sel4_server_t *srv,
                                        uint32_t opcode,
                                        sel4_handler_fn fn,
                                        void *ctx)
{
    if (srv->handler_count >= SEL4_SERVER_MAX_HANDLERS)
        return -1;
    srv->handlers[srv->handler_count].opcode = opcode;
    srv->handlers[srv->handler_count].fn     = fn;
    srv->handlers[srv->handler_count].ctx    = ctx;
    srv->handler_count++;
    return 0;
}

/* ── Dispatch ────────────────────────────────────────────────────────────
 *
 * sel4_server_dispatch() — route an incoming message to its handler.
 *
 * Performs a linear scan of the handler table for req->opcode.
 * O(n) where n <= SEL4_SERVER_MAX_HANDLERS (32).
 *
 * On a match: calls the handler and returns its error code.
 * On no match: sets rep->opcode = SEL4_ERR_INVALID_OP, rep->length = 0,
 *              and returns SEL4_ERR_INVALID_OP.
 *
 * The caller (sel4_server_run) is responsible for sending the reply.
 */
static inline uint32_t sel4_server_dispatch(sel4_server_t *srv,
                                             sel4_badge_t badge,
                                             const sel4_msg_t *req,
                                             sel4_msg_t *rep)
{
    uint32_t any_idx = SEL4_SERVER_MAX_HANDLERS; /* sentinel: no ANY found */

    for (uint32_t i = 0; i < srv->handler_count; i++) {
        if (srv->handlers[i].opcode == SEL4_SERVER_OPCODE_ANY) {
            if (any_idx == SEL4_SERVER_MAX_HANDLERS)
                any_idx = i;   /* remember first ANY; keep scanning for exact */
            continue;
        }
        if (srv->handlers[i].opcode == req->opcode) {
            uint32_t rc = srv->handlers[i].fn(badge, req, rep,
                                               srv->handlers[i].ctx);
            rep->opcode = rc;   /* propagate status into reply opcode field */
            return rc;
        }
    }

    /* No exact match — fall through to the ANY (wildcard) handler if present. */
    if (any_idx < SEL4_SERVER_MAX_HANDLERS) {
        uint32_t rc = srv->handlers[any_idx].fn(badge, req, rep,
                                                 srv->handlers[any_idx].ctx);
        rep->opcode = rc;
        return rc;
    }

    rep->opcode = SEL4_ERR_INVALID_OP;
    rep->length = 0;
    return SEL4_ERR_INVALID_OP;
}

/* ── Main server loop ────────────────────────────────────────────────────
 *
 * sel4_server_run() — enter the server receive/dispatch/reply loop.
 *
 * NEVER RETURNS.  The compiler can observe the unconditional while(1).
 *
 * Protocol:
 *   Iteration 1: sel4_recv (no pending reply yet)
 *   Iteration N: sel4_reply_recv (atomically reply + receive next message)
 *
 * This matches the seL4 ReplyRecv fast-path: the kernel delivers the reply
 * to the waiting caller and schedules the next incoming caller in one step,
 * avoiding an extra kernel crossing.
 *
 * rep is populated by sel4_server_dispatch and sent on the *following*
 * sel4_reply_recv call (i.e., replies are delayed by one iteration,
 * which is the standard seL4 server pattern).
 */
static inline void sel4_server_run(sel4_server_t *srv)
{
    sel4_msg_t   req  = {0};
    sel4_msg_t   rep  = {0};
    sel4_badge_t badge;
    int          first = 1;

    while (1) {
        if (first) {
            badge = sel4_recv(srv->ep, &req);
            first = 0;
        } else {
            badge = sel4_reply_recv(srv->ep, &rep, &req);
        }

        /* Reset reply before dispatching so handlers start from a clean slate. */
        rep.opcode = 0;
        rep.length = 0;

        sel4_server_dispatch(srv, badge, &req, &rep);
        /* rep is sent on the next sel4_reply_recv iteration. */
    }
}
