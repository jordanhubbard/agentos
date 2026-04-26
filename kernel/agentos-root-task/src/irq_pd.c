/*
 * irq_pd.c — agentOS Generic IRQ Protection Domain
 *
 * OS-neutral IPC IRQ routing service. Provides REGISTER/UNREGISTER/ACKNOWLEDGE/
 * MASK/UNMASK/STATUS over seL4 IPC.
 *
 * IPC Protocol:
 *   IRQ_OP_REGISTER    (0x1050) — data[4..7]=irq_num data[8..11]=notify_ch → ok
 *   IRQ_OP_UNREGISTER  (0x1051) — data[4..7]=irq_num → ok
 *   IRQ_OP_ACKNOWLEDGE (0x1052) — data[4..7]=irq_num → ok
 *   IRQ_OP_MASK        (0x1053) — data[4..7]=irq_num → ok
 *   IRQ_OP_UNMASK      (0x1054) — data[4..7]=irq_num → ok
 *   IRQ_OP_STATUS      (0x1055) — data[4..7]=irq_num → masked, pending, count
 *
 * Priority: 230
 *
 * E5-S8: migrated from Microkit to raw seL4 IPC.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "contracts/irq_contract.h"
#include <stdbool.h>

/* ── State ───────────────────────────────────────────────────────────────── */
static bool     s_registered[IRQ_MAX_IRQS];
static bool     s_masked[IRQ_MAX_IRQS];
static bool     s_pending[IRQ_MAX_IRQS];
static uint32_t s_notify_ch[IRQ_MAX_IRQS];
static uint32_t s_count[IRQ_MAX_IRQS];

/* ── msg helpers ─────────────────────────────────────────────────────────── */

#ifndef AGENTOS_IPC_HELPERS_DEFINED
#define AGENTOS_IPC_HELPERS_DEFINED
static inline uint32_t msg_u32(const sel4_msg_t *m, uint32_t off) {
    uint32_t v = 0;
    if (off + 4u <= SEL4_MSG_DATA_BYTES) {
        v  = (uint32_t)m->data[off]; v |= (uint32_t)m->data[off+1]<<8;
        v |= (uint32_t)m->data[off+2]<<16; v |= (uint32_t)m->data[off+3]<<24;
    }
    return v;
}
static inline void rep_u32(sel4_msg_t *m, uint32_t off, uint32_t v) {
    if (off + 4u <= SEL4_MSG_DATA_BYTES) {
        m->data[off]=(uint8_t)v; m->data[off+1]=(uint8_t)(v>>8);
        m->data[off+2]=(uint8_t)(v>>16); m->data[off+3]=(uint8_t)(v>>24);
    }
}
#endif /* AGENTOS_IPC_HELPERS_DEFINED */

/* ── Handlers ────────────────────────────────────────────────────────────── */

static uint32_t h_register(sel4_badge_t b, const sel4_msg_t *req,
                             sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t irq_num   = msg_u32(req, 4);
    uint32_t notify_ch = msg_u32(req, 8);
    if (irq_num >= IRQ_MAX_IRQS) {
        rep_u32(rep, 0, IRQ_ERR_BAD_IRQ); rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }
    if (s_registered[irq_num]) {
        rep_u32(rep, 0, IRQ_ERR_ALREADY_REG); rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }
    s_registered[irq_num] = true;
    s_masked[irq_num]     = true;
    s_notify_ch[irq_num]  = notify_ch;
    s_count[irq_num]      = 0;
    s_pending[irq_num]    = false;
    rep_u32(rep, 0, IRQ_OK); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_unregister(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t irq_num = msg_u32(req, 4);
    if (irq_num >= IRQ_MAX_IRQS || !s_registered[irq_num]) {
        rep_u32(rep, 0, IRQ_ERR_BAD_SLOT); rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }
    s_masked[irq_num]     = true;
    s_registered[irq_num] = false;
    rep_u32(rep, 0, IRQ_OK); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_acknowledge(sel4_badge_t b, const sel4_msg_t *req,
                                sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t irq_num = msg_u32(req, 4);
    if (irq_num >= IRQ_MAX_IRQS || !s_registered[irq_num]) {
        rep_u32(rep, 0, IRQ_ERR_BAD_SLOT); rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }
    s_pending[irq_num] = false;
    /* TODO Phase 2.5: seL4_IRQHandler_Ack(irq_cap[irq_num]); */
    rep_u32(rep, 0, IRQ_OK); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_mask(sel4_badge_t b, const sel4_msg_t *req,
                         sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t irq_num = msg_u32(req, 4);
    if (irq_num >= IRQ_MAX_IRQS || !s_registered[irq_num]) {
        rep_u32(rep, 0, IRQ_ERR_BAD_SLOT); rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }
    s_masked[irq_num] = true;
    rep_u32(rep, 0, IRQ_OK); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_unmask(sel4_badge_t b, const sel4_msg_t *req,
                           sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t irq_num = msg_u32(req, 4);
    if (irq_num >= IRQ_MAX_IRQS || !s_registered[irq_num]) {
        rep_u32(rep, 0, IRQ_ERR_BAD_SLOT); rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }
    s_masked[irq_num] = false;
    rep_u32(rep, 0, IRQ_OK); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_status(sel4_badge_t b, const sel4_msg_t *req,
                           sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t irq_num = msg_u32(req, 4);
    if (irq_num >= IRQ_MAX_IRQS || !s_registered[irq_num]) {
        rep_u32(rep, 0, IRQ_ERR_BAD_SLOT); rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }
    rep_u32(rep, 0,  IRQ_OK);
    rep_u32(rep, 4,  s_masked[irq_num]  ? 1u : 0u);
    rep_u32(rep, 8,  s_pending[irq_num] ? 1u : 0u);
    rep_u32(rep, 12, s_count[irq_num]);
    rep->length = 16;
    return SEL4_ERR_OK;
}

void irq_pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    for (uint32_t i = 0; i < IRQ_MAX_IRQS; i++) {
        s_registered[i] = false;
        s_masked[i]     = true;
        s_pending[i]    = false;
        s_notify_ch[i]  = 0;
        s_count[i]      = 0;
    }
    sel4_dbg_puts("[irq_pd] ready\n");

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, IRQ_OP_REGISTER,    h_register,    (void *)0);
    sel4_server_register(&srv, IRQ_OP_UNREGISTER,  h_unregister,  (void *)0);
    sel4_server_register(&srv, IRQ_OP_ACKNOWLEDGE, h_acknowledge, (void *)0);
    sel4_server_register(&srv, IRQ_OP_MASK,        h_mask,        (void *)0);
    sel4_server_register(&srv, IRQ_OP_UNMASK,      h_unmask,      (void *)0);
    sel4_server_register(&srv, IRQ_OP_STATUS,      h_status,      (void *)0);
    sel4_server_run(&srv);
}
