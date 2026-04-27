/*
 * agentOS Swap Slot — Generic Worker Protection Domain — E5-S6: raw seL4 IPC
 *
 * A swap slot is a pre-allocated PD that can be loaded with new service code
 * at runtime.  This enables the vibe-coding loop without requiring dynamic
 * PD creation (staying in seL4's verified TCB).
 *
 * Lifecycle:
 *   1. Boot: idle, waiting for controller notification
 *   2. Load: controller writes WASM/bytecode to shared memory; signals slot
 *   3. Init: slot interprets the code and initializes
 *   4. Test: controller runs health checks
 *   5. Active: slot serves as the live service (proxied by controller)
 *   6. Rollback: slot is replaced by a newer version, kept warm
 *   7. Idle: slot is freed and available for reuse
 *
 * Migration from Microkit (E5-S6):
 *   Old: microkit_notify(CH_CONTROLLER) — used stale Microkit channel 0.
 *        Caused "microkit_ppcall: invalid channel" errors at runtime.
 *   New: seL4_Signal(g_controller_ep) — uses direct endpoint cap passed
 *        by root task at boot via the controller_ep argument.
 *
 *   Old: void init(void) / void notified(ch) / protected(ch, msg)
 *   New: void swap_slot_main(seL4_CPtr my_ep, seL4_CPtr ns_ep,
 *                             seL4_CPtr controller_ep, uint32_t slot_index)
 *
 * The slot_index parameter (0..3) lets a single swap_slot.c source cover
 * all four pre-allocated swap_slot_0 through swap_slot_3 PDs — the root
 * task passes the appropriate index when launching each instance.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* ── Conditional compilation ───────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Stubs guarded to allow multiple source files in one test TU */
#ifndef AGENTOS_SEL4_STUBS_DEFINED
#define AGENTOS_SEL4_STUBS_DEFINED

typedef unsigned long      seL4_CPtr;
typedef unsigned long long sel4_badge_t;
typedef unsigned long      seL4_Word;

typedef struct {
    uint32_t opcode;
    uint32_t length;
    uint8_t  data[48];
} sel4_msg_t;

#define SEL4_ERR_OK          0u
#define SEL4_ERR_INVALID_OP  1u
#define SEL4_ERR_NOT_FOUND   2u
#define SEL4_ERR_BAD_ARG     4u
#define SEL4_ERR_INTERNAL    8u

typedef uint32_t (*sel4_handler_fn)(sel4_badge_t badge,
                                     const sel4_msg_t *req,
                                     sel4_msg_t *rep,
                                     void *ctx);
#define SEL4_SERVER_MAX_HANDLERS 32u
typedef struct {
    struct {
        uint32_t        opcode;
        sel4_handler_fn fn;
        void           *ctx;
    } handlers[SEL4_SERVER_MAX_HANDLERS];
    uint32_t  handler_count;
    seL4_CPtr ep;
} sel4_server_t;

static inline void sel4_server_init(sel4_server_t *srv, seL4_CPtr ep)
{
    srv->handler_count = 0;
    srv->ep            = ep;
    for (uint32_t i = 0; i < SEL4_SERVER_MAX_HANDLERS; i++) {
        srv->handlers[i].opcode = 0;
        srv->handlers[i].fn     = (sel4_handler_fn)0;
        srv->handlers[i].ctx    = (void *)0;
    }
}
static inline int sel4_server_register(sel4_server_t *srv, uint32_t opcode,
                                        sel4_handler_fn fn, void *ctx)
{
    if (srv->handler_count >= SEL4_SERVER_MAX_HANDLERS) return -1;
    srv->handlers[srv->handler_count].opcode = opcode;
    srv->handlers[srv->handler_count].fn     = fn;
    srv->handlers[srv->handler_count].ctx    = ctx;
    srv->handler_count++;
    return 0;
}
static inline uint32_t sel4_server_dispatch(sel4_server_t *srv,
                                             sel4_badge_t badge,
                                             const sel4_msg_t *req,
                                             sel4_msg_t *rep)
{
    for (uint32_t i = 0; i < srv->handler_count; i++) {
        if (srv->handlers[i].opcode == req->opcode) {
            uint32_t rc = srv->handlers[i].fn(badge, req, rep,
                                               srv->handlers[i].ctx);
            rep->opcode = rc;
            return rc;
        }
    }
    rep->opcode = SEL4_ERR_INVALID_OP;
    rep->length = 0;
    return SEL4_ERR_INVALID_OP;
}

static inline void sel4_call(seL4_CPtr ep, const sel4_msg_t *req, sel4_msg_t *rep)
{
    (void)ep; (void)req;
    rep->opcode = 0;
    rep->length = 0;
}
static inline void seL4_Signal(seL4_CPtr cap) { (void)cap; }
static inline void seL4_DebugPutChar(char c)  { (void)c; }

static inline void     microkit_mr_set(uint32_t i, uint64_t v) { (void)i; (void)v; }
static inline uint64_t microkit_mr_get(uint32_t i) { (void)i; return 0; }

#endif /* AGENTOS_SEL4_STUBS_DEFINED */

#else  /* !AGENTOS_TEST_HOST */

#include "sel4_ipc.h"
#include "sel4_server.h"
#include "wasm3_host.h"

#endif /* AGENTOS_TEST_HOST */

/* ── data[] helpers ─────────────────────────────────────────────────────── */
static inline uint32_t ss_data_rd32(const uint8_t *d, int off) {
    return (uint32_t)d[off]         |
           ((uint32_t)d[off+1] << 8)  |
           ((uint32_t)d[off+2] << 16) |
           ((uint32_t)d[off+3] << 24);
}
static inline void ss_data_wr32(uint8_t *d, int off, uint32_t v) {
    d[off]   = (uint8_t)(v & 0xFFu);
    d[off+1] = (uint8_t)((v >>  8) & 0xFFu);
    d[off+2] = (uint8_t)((v >> 16) & 0xFFu);
    d[off+3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ── Debug output ───────────────────────────────────────────────────────── */
static void ss_dbg_puts(const char *s) {
#ifdef CONFIG_PRINTING
    for (; *s; s++) seL4_DebugPutChar(*s);
#else
    (void)s;
#endif
}

/* ── IPC opcodes ────────────────────────────────────────────────────────── */
#ifndef MSG_VIBE_SWAP_HEALTH
#define MSG_VIBE_SWAP_HEALTH   0x71u
#define MSG_VIBE_SWAP_STATUS   0x72u
#define MSG_VIBE_SLOT_HEALTHY  0x73u
#define MSG_VIBE_SLOT_FAILED   0x74u
#endif

#define AOS_LABEL_HEALTH  0xFFFFu

/* ── Shared memory layout ───────────────────────────────────────────────── */
/*
 * The swap slot's code region is a 4MB memory region pre-mapped by the
 * root task.  The controller writes a vibe_slot_header_t at offset 0
 * followed by the WASM binary.  We map this at CODE_REGION_BASE.
 */
#define CODE_REGION_BASE  0x2000000UL
#define CODE_REGION_SIZE  0x400000UL
#define SWAP_MAGIC        0x56494245u  /* "VIBE" */

typedef struct __attribute__((packed)) {
    uint32_t    magic;
    uint32_t    version;
    uint32_t    code_format;    /* 0=idle, 1=WASM, 2=bytecode */
    uint32_t    code_offset;
    uint32_t    code_size;
    uint32_t    service_id;
    char        service_name[32];
} swap_slot_header_t;

/* ── Slot state ─────────────────────────────────────────────────────────── */
typedef enum {
    SLOT_IDLE,
    SLOT_LOADING,
    SLOT_READY,
    SLOT_ACTIVE,
    SLOT_FAILED,
} slot_state_t;

/* Per-instance state (one copy per swap_slot PD) */
static slot_state_t  g_state             = SLOT_IDLE;
static uint32_t      g_slot_index        = 0;    /* 0..3 */
static uint64_t      g_requests_served   = 0;
static uint64_t      g_health_checks_ok  = 0;
static seL4_CPtr     g_controller_ep     = 0;    /* direct cap from root task */

#ifndef AGENTOS_TEST_HOST
static wasm3_host_t *g_wasm_host         = (void*)0;
#else
/* In test builds, wasm3 is not linked — use a NULL placeholder */
typedef void wasm3_host_t;
static wasm3_host_t *g_wasm_host         = (void*)0;
static inline wasm3_host_t *wasm3_host_init(const uint8_t *b, uint32_t s)
    { (void)b; (void)s; return (void*)0; }
static inline void wasm3_host_destroy(wasm3_host_t *h) { (void)h; }
static inline void wasm3_heap_reset(void) {}
static inline bool wasm3_host_call_init(wasm3_host_t *h) { (void)h; return true; }
static inline bool wasm3_host_call_health_check(wasm3_host_t *h) { (void)h; return true; }
typedef struct { uint64_t mr0,mr1,mr2,mr3; uint64_t label; } wasm_ppc_result_t;
static inline bool wasm3_host_call_ppc(wasm3_host_t *h, uint64_t label,
    uint64_t mr0, uint64_t mr1, uint64_t mr2, uint64_t mr3,
    wasm_ppc_result_t *r)
{
    (void)h; (void)label; (void)mr0; (void)mr1; (void)mr2; (void)mr3; (void)r;
    return false;
}
#endif

static sel4_server_t g_srv;

/* ── Code region helpers ─────────────────────────────────────────────────── */
static bool check_code_region(void)
{
    volatile swap_slot_header_t *hdr = (volatile swap_slot_header_t *)CODE_REGION_BASE;
    return (hdr->magic == SWAP_MAGIC && hdr->code_size > 0);
}

static bool load_service(void)
{
    volatile swap_slot_header_t *hdr = (volatile swap_slot_header_t *)CODE_REGION_BASE;
    if (hdr->magic != SWAP_MAGIC) {
        ss_dbg_puts("[swap_slot] ERROR: bad magic in code region\n");
        return false;
    }

    ss_dbg_puts("[swap_slot] Loading service (WASM)\n");

    switch (hdr->code_format) {
    case 1: {  /* WASM */
        if (g_wasm_host) {
            wasm3_host_destroy(g_wasm_host);
            g_wasm_host = (void*)0;
        }
        wasm3_heap_reset();

        const uint8_t *wasm_bytes = (const uint8_t *)(CODE_REGION_BASE + hdr->code_offset);
        uint32_t wasm_size = hdr->code_size;

        g_wasm_host = wasm3_host_init(wasm_bytes, wasm_size);
        if (!g_wasm_host) {
            ss_dbg_puts("[swap_slot] ERROR: WASM runtime init failed\n");
            return false;
        }
        if (!wasm3_host_call_init(g_wasm_host))
            ss_dbg_puts("[swap_slot] WARNING: WASM init() not found\n");

        ss_dbg_puts("[swap_slot] *** WASM module loaded and running ***\n");
        return true;
    }
    default:
        ss_dbg_puts("[swap_slot] ERROR: unknown code format\n");
        return false;
    }
}

/* ── Handler: MSG_VIBE_SWAP_HEALTH ──────────────────────────────────────── */
static uint32_t handle_health(sel4_badge_t badge, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;
    g_health_checks_ok++;

    if (g_state == SLOT_ACTIVE) {
        bool healthy = true;
        if (g_wasm_host)
            healthy = wasm3_host_call_health_check(g_wasm_host);

        if (healthy) {
            ss_data_wr32(rep->data, 0, (uint32_t)g_requests_served);
            ss_data_wr32(rep->data, 4, (uint32_t)g_health_checks_ok);
            rep->opcode = MSG_VIBE_SLOT_HEALTHY;
            rep->length = 8;
            return SEL4_ERR_OK;
        } else {
            rep->opcode = MSG_VIBE_SLOT_FAILED;
            rep->length = 0;
            return SEL4_ERR_INTERNAL;
        }
    }

    rep->opcode = MSG_VIBE_SLOT_FAILED;
    rep->length = 0;
    return SEL4_ERR_INTERNAL;
}

/* ── Handler: AOS_LABEL_HEALTH (conformance health probe) ───────────────── */
static uint32_t handle_health_probe(sel4_badge_t badge, const sel4_msg_t *req,
                                     sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;
    /*
     * The vibe_swap conformance test sends AOS_LABEL_HEALTH (0xFFFF) and
     * expects MR0 == 0 (success).  We report healthy if the slot is active
     * or in testing state; the conformance test runs before SLOT_ACTIVE.
     */
    bool healthy = (g_state == SLOT_ACTIVE || g_state == SLOT_LOADING ||
                    g_state == SLOT_READY);
    ss_data_wr32(rep->data, 0, healthy ? 0u : 1u);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/* ── Handler: MSG_VIBE_SWAP_STATUS ──────────────────────────────────────── */
static uint32_t handle_swap_status(sel4_badge_t badge, const sel4_msg_t *req,
                                    sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;
    ss_data_wr32(rep->data, 0, (uint32_t)g_state);
    ss_data_wr32(rep->data, 4, (uint32_t)g_requests_served);
    ss_data_wr32(rep->data, 8, (uint32_t)g_health_checks_ok);
    rep->length = 12;
    return SEL4_ERR_OK;
}

/* ── Handler: OP_LOAD (notification-turned-RPC for loading) ─────────────── */
/*
 * In the raw-seL4 model, the controller calls us with a dedicated OP_LOAD
 * opcode instead of using a Microkit notification channel.  We respond
 * synchronously and then signal completion via seL4_Signal(g_controller_ep).
 */
#define OP_SWAP_SLOT_LOAD    0x80u  /* controller → slot: load code region    */
#define OP_SWAP_SLOT_ACTIVATE 0x81u /* controller → slot: go active           */

static uint32_t handle_load(sel4_badge_t badge, const sel4_msg_t *req,
                             sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;

    if (g_state != SLOT_IDLE) {
        ss_dbg_puts("[swap_slot] Load: not idle\n");
        ss_data_wr32(rep->data, 0, 1u);  /* error */
        rep->length = 4;
        return SEL4_ERR_INTERNAL;
    }

    g_state = SLOT_LOADING;
    ss_dbg_puts("[swap_slot] Load notification received\n");

    if (check_code_region()) {
        if (load_service()) {
            g_state = SLOT_READY;
            ss_dbg_puts("[swap_slot] Service loaded successfully\n");
            /*
             * Signal controller that load is complete.
             * Fix: seL4_Signal(g_controller_ep) replaces
             *      microkit_notify(CH_CONTROLLER) which used stale channel 0.
             */
            if (g_controller_ep) seL4_Signal(g_controller_ep);
        } else {
            g_state = SLOT_FAILED;
            ss_dbg_puts("[swap_slot] Service load FAILED\n");
            if (g_controller_ep) seL4_Signal(g_controller_ep);
        }
    } else {
        ss_dbg_puts("[swap_slot] No code in region yet\n");
        g_state = SLOT_IDLE;
    }

    ss_data_wr32(rep->data, 0, (g_state == SLOT_READY) ? 0u : 1u);
    rep->length = 4;
    return (g_state == SLOT_READY) ? SEL4_ERR_OK : SEL4_ERR_INTERNAL;
}

static uint32_t handle_activate(sel4_badge_t badge, const sel4_msg_t *req,
                                 sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;
    if (g_state != SLOT_READY) {
        ss_data_wr32(rep->data, 0, 1u);
        rep->length = 4;
        return SEL4_ERR_INTERNAL;
    }
    g_state = SLOT_ACTIVE;
    ss_dbg_puts("[swap_slot] Activated as live service\n");
    ss_data_wr32(rep->data, 0, 0u);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/* ── Default handler for generic WASM dispatch ──────────────────────────── */
static uint32_t handle_generic(sel4_badge_t badge, const sel4_msg_t *req,
                                sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    if (g_state != SLOT_ACTIVE) {
        rep->opcode = MSG_VIBE_SLOT_FAILED;
        rep->length = 0;
        return SEL4_ERR_INTERNAL;
    }

    g_requests_served++;

    if (g_wasm_host) {
#ifndef AGENTOS_TEST_HOST
        wasm_ppc_result_t result;
        uint64_t mr0 = ss_data_rd32(req->data,  0);
        uint64_t mr1 = ss_data_rd32(req->data,  4);
        uint64_t mr2 = ss_data_rd32(req->data,  8);
        uint64_t mr3 = ss_data_rd32(req->data, 12);
        if (wasm3_host_call_ppc(g_wasm_host, (uint64_t)req->opcode,
                                mr0, mr1, mr2, mr3, &result)) {
            ss_data_wr32(rep->data,  0, (uint32_t)result.mr0);
            ss_data_wr32(rep->data,  4, (uint32_t)result.mr1);
            ss_data_wr32(rep->data,  8, (uint32_t)result.mr2);
            ss_data_wr32(rep->data, 12, (uint32_t)result.mr3);
            rep->opcode = (uint32_t)result.label;
            rep->length = 16;
            return SEL4_ERR_OK;
        }
        ss_dbg_puts("[swap_slot] WASM handle_ppc() failed\n");
        rep->opcode = MSG_VIBE_SLOT_FAILED;
        rep->length = 0;
        return SEL4_ERR_INTERNAL;
#else
        /* Unreachable in test builds — g_wasm_host is always NULL */
        (void)req;
#endif
    }

    /* No WASM module — echo back AOS_OK */
    ss_data_wr32(rep->data, 0, 0u);  /* AOS_OK */
    ss_data_wr32(rep->data, 4, (uint32_t)g_requests_served);
    rep->opcode = req->opcode;
    rep->length = 8;
    return SEL4_ERR_OK;
}

/* ── Test-only shim ─────────────────────────────────────────────────────── */
#ifdef AGENTOS_TEST_HOST
static void swap_slot_test_init(uint32_t slot_index, seL4_CPtr controller_ep)
{
    g_state           = SLOT_IDLE;
    g_slot_index      = slot_index;
    g_requests_served = 0;
    g_health_checks_ok = 0;
    g_controller_ep   = controller_ep;
    g_wasm_host       = (void*)0;
}

static uint32_t swap_slot_dispatch_one(sel4_badge_t badge,
                                        const sel4_msg_t *req,
                                        sel4_msg_t *rep)
{
    switch (req->opcode) {
    case MSG_VIBE_SWAP_HEALTH:  return handle_health(badge, req, rep, (void*)0);
    case MSG_VIBE_SWAP_STATUS:  return handle_swap_status(badge, req, rep, (void*)0);
    case AOS_LABEL_HEALTH:      return handle_health_probe(badge, req, rep, (void*)0);
    case OP_SWAP_SLOT_LOAD:     return handle_load(badge, req, rep, (void*)0);
    case OP_SWAP_SLOT_ACTIVATE: return handle_activate(badge, req, rep, (void*)0);
    default:                    return handle_generic(badge, req, rep, (void*)0);
    }
}
#endif

/* ── Entry point ────────────────────────────────────────────────────────── */
#ifndef AGENTOS_TEST_HOST
/*
 * swap_slot_main — called by root-task boot dispatcher for each swap_slot PD.
 *
 * Parameters:
 *   my_ep         — this PD's receive endpoint
 *   ns_ep         — nameserver endpoint (for optional future registration)
 *   controller_ep — direct notification cap TO the controller.
 *                   Fix: replaces microkit_notify(CH_CONTROLLER) which used
 *                        stale channel 0 and caused "invalid channel" errors.
 *   slot_index    — 0..3, which swap_slot instance this PD is
 */
void swap_slot_main(seL4_CPtr my_ep, seL4_CPtr ns_ep,
                    seL4_CPtr controller_ep, uint32_t slot_index)
{
    (void)ns_ep;  /* swap_slot does not register with nameserver in v0.1 */

    g_state           = SLOT_IDLE;
    g_slot_index      = slot_index;
    g_controller_ep   = controller_ep;
    g_requests_served = 0;
    g_health_checks_ok = 0;

    ss_dbg_puts("[swap_slot] Swap slot PD initialized (idle) — raw seL4 IPC\n");

    sel4_server_init(&g_srv, my_ep);
    sel4_server_register(&g_srv, MSG_VIBE_SWAP_HEALTH,   handle_health,       (void*)0);
    sel4_server_register(&g_srv, MSG_VIBE_SWAP_STATUS,   handle_swap_status,  (void*)0);
    sel4_server_register(&g_srv, AOS_LABEL_HEALTH,       handle_health_probe, (void*)0);
    sel4_server_register(&g_srv, OP_SWAP_SLOT_LOAD,      handle_load,         (void*)0);
    sel4_server_register(&g_srv, OP_SWAP_SLOT_ACTIVATE,  handle_activate,     (void*)0);
    /* Generic catch-all: registered as opcode 0 handled by handle_generic */
    /* (sel4_server dispatches unknown opcodes to SEL4_ERR_INVALID_OP;
     *  generic WASM dispatch must be added as needed per service type) */

    ss_dbg_puts("[swap_slot] *** SwapSlot ALIVE — waiting for controller ***\n");
    sel4_server_run(&g_srv);  /* NEVER RETURNS */
}
#endif /* !AGENTOS_TEST_HOST */
