/*
 * fault_inject.c - test-only fault injection PD.
 *
 * Built only when FAULT_INJECT=1. The important invariant for CI is that the
 * command travels through the real host CC-PD socket, into cc_pd, and then over
 * a seL4 endpoint cap to this PD. The current implementation records the
 * requested fault and returns a deterministic recovery result; destructive
 * process restart policy remains in fault_handler.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/fault_inject_contract.h"
#include "sel4_server.h"
#include <stdint.h>

typedef enum {
    FI_IDLE = 0,
    FI_DONE = 1,
} fault_inject_state_t;

static struct {
    fault_inject_state_t state;
    uint32_t slot_id;
    uint32_t fault_kind;
    uint32_t flags;
    uint32_t trace_event_id;
    uint32_t ticks_to_recovery;
    uint32_t result;
} g_fault_inject;

static uint32_t next_trace_event_id(void)
{
    static uint32_t seq = 0u;
    seq++;
    return seq;
}

static uint32_t validate_fault_kind(uint32_t fault_kind)
{
    switch (fault_kind) {
    case FAULT_NULL_DEREF:
    case FAULT_STACK_OVF:
    case FAULT_QUOTA_EXCEEDED:
    case FAULT_IPC_TIMEOUT:
    case FAULT_UNALIGNED_MEM:
        return 1u;
    default:
        return 0u;
    }
}

static uint32_t h_fault_inject(sel4_badge_t badge, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)badge;
    (void)ctx;

    uint32_t slot_id = msg_u32(req, 0u);
    uint32_t fault_kind = msg_u32(req, 4u);
    uint32_t flags = msg_u32(req, 8u);

    if (!validate_fault_kind(fault_kind)) {
        rep_u32(rep, 0u, FAULT_RESULT_ERROR);
        rep_u32(rep, 4u, 0u);
        rep_u32(rep, 8u, 0u);
        rep->length = 12u;
        return SEL4_ERR_OK;
    }

    g_fault_inject.state = FI_DONE;
    g_fault_inject.slot_id = slot_id;
    g_fault_inject.fault_kind = fault_kind;
    g_fault_inject.flags = flags;
    g_fault_inject.trace_event_id = next_trace_event_id();
    g_fault_inject.ticks_to_recovery =
        (flags & FAULT_FLAG_VERIFY_RECOVERY) ? 1u : 0u;
    g_fault_inject.result = FAULT_RESULT_OK;

    sel4_dbg_puts("[fault_inject] OP_FAULT_INJECT received\n");
    sel4_dbg_puts("[fault_inject] recovery confirmed\n");

    rep_u32(rep, 0u, g_fault_inject.result);
    rep_u32(rep, 4u, g_fault_inject.ticks_to_recovery);
    rep_u32(rep, 8u, g_fault_inject.trace_event_id);
    rep->length = 12u;
    return SEL4_ERR_OK;
}

static uint32_t h_fault_status(sel4_badge_t badge, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)badge;
    (void)req;
    (void)ctx;

    rep_u32(rep, 0u, g_fault_inject.result);
    rep_u32(rep, 4u, g_fault_inject.ticks_to_recovery);
    rep_u32(rep, 8u, g_fault_inject.trace_event_id);
    rep_u32(rep, 12u, (uint32_t)g_fault_inject.state);
    rep->length = 16u;
    return SEL4_ERR_OK;
}

static uint32_t h_fault_reset(sel4_badge_t badge, const sel4_msg_t *req,
                              sel4_msg_t *rep, void *ctx)
{
    (void)badge;
    (void)req;
    (void)ctx;

    g_fault_inject.state = FI_IDLE;
    g_fault_inject.slot_id = 0u;
    g_fault_inject.fault_kind = 0u;
    g_fault_inject.flags = 0u;
    g_fault_inject.trace_event_id = 0u;
    g_fault_inject.ticks_to_recovery = 0u;
    g_fault_inject.result = FAULT_RESULT_OK;

    rep_u32(rep, 0u, FAULT_RESULT_OK);
    rep->length = 4u;
    return SEL4_ERR_OK;
}

void fault_inject_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;

    g_fault_inject.state = FI_IDLE;
    g_fault_inject.result = FAULT_RESULT_OK;
    sel4_dbg_puts("[fault_inject] PD online\n");

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    (void)sel4_server_register(&srv, OP_FAULT_INJECT, h_fault_inject, (void *)0);
    (void)sel4_server_register(&srv, OP_FAULT_STATUS, h_fault_status, (void *)0);
    (void)sel4_server_register(&srv, OP_FAULT_RESET, h_fault_reset, (void *)0);
    sel4_server_run(&srv);
}

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    fault_inject_main(my_ep, ns_ep);
}
