/*
 * agentOS Vibe Swap — Kernel-side hot-swap support — E5-S6: raw seL4 IPC
 *
 * This module enables the vibe-coding loop:
 *   Agent generates new service code → validates → swaps live service
 *
 * Architecture:
 *   The VibeEngine (userspace service) handles the protocol:
 *   propose/validate/sandbox/approve. Once approved, the controller
 *   executes the swap at the kernel level via vibe_swap_begin().
 *
 * Swap mechanism:
 *   seL4 PDs are static — we can't create new ones at runtime.
 *   Instead, we use a "swap slot" model:
 *
 *   1. The system pre-allocates N swap-slot PDs
 *   2. Each slot is an idle worker PD with an endpoint to the controller
 *   3. On swap: controller loads new code into a swap slot PD
 *   4. Controller redirects service routing to the new PD
 *   5. Old PD becomes the rollback slot (kept warm for quick revert)
 *
 *   This stays within seL4's verified TCB — no dynamic PD creation,
 *   no Microkit channels, just endpoint caps distributed at boot.
 *
 * Migration from Microkit (E5-S6):
 *   Old: microkit_notify(slots[slot].channel) where channel was a stale
 *        Microkit integer — caused "invalid channel '30'" runtime errors.
 *   New: seL4_Signal(slot_ep[slot]) where slot_ep[] holds direct endpoint
 *        caps distributed by root task at boot via swap_slot_eps arg.
 *
 *   Old: PPCALL_DONATE(slots[slot].channel, ...) — same stale channel bug.
 *   New: sel4_call(slot_ep[slot], &req, &rep)
 *
 * Entry point: void vibe_swap_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
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

#include <stdbool.h>
#include "sel4_ipc.h"
#include "sel4_server.h"
#include "sel4_client.h"

#endif /* AGENTOS_TEST_HOST */

/* ── data[] helpers ─────────────────────────────────────────────────────── */
static inline uint32_t vs_data_rd32(const uint8_t *d, int off) {
    return (uint32_t)d[off]         |
           ((uint32_t)d[off+1] << 8)  |
           ((uint32_t)d[off+2] << 16) |
           ((uint32_t)d[off+3] << 24);
}
static inline void vs_data_wr32(uint8_t *d, int off, uint32_t v) {
    d[off]   = (uint8_t)(v & 0xFFu);
    d[off+1] = (uint8_t)((v >>  8) & 0xFFu);
    d[off+2] = (uint8_t)((v >> 16) & 0xFFu);
    d[off+3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ── Debug output ───────────────────────────────────────────────────────── */
static void vs_dbg_puts(const char *s) {
    for (; *s; s++) seL4_DebugPutChar(*s);
}

/* ── Memory barrier ─────────────────────────────────────────────────────── */
#ifndef agentos_wmb
#ifdef AGENTOS_TEST_HOST
#define agentos_wmb() ((void)0)
#else
#define agentos_wmb() __asm__ volatile("dmb st" ::: "memory")
#endif
#endif

/* ── ABI opcodes ────────────────────────────────────────────────────────── */
#ifndef MSG_VIBE_SWAP_HEALTH
#define MSG_VIBE_SWAP_HEALTH   0x71u
#define MSG_VIBE_SWAP_STATUS   0x72u
#define MSG_VIBE_SLOT_HEALTHY  0x73u
#define MSG_VIBE_SLOT_FAILED   0x74u
#endif

/* ABI label constants (mirrors agentos_service_abi.h) */
#define AOS_LABEL_HEALTH    0xFFFFu
#define STORAGE_OP_WRITE    0x30u
#define STORAGE_OP_READ     0x31u

/* ── Swap slot configuration ────────────────────────────────────────────── */
#define MAX_SWAP_SLOTS       4

/* Controller-side code region virtual addresses (set by root-task boot) */
extern uintptr_t swap_code_ctrl_0;
extern uintptr_t swap_code_ctrl_1;
extern uintptr_t swap_code_ctrl_2;
extern uintptr_t swap_code_ctrl_3;

#define SWAP_CODE_REGION_SIZE   0x400000UL   /* 4MB per slot */
#define SWAP_HEADER_SIZE        64           /* Cache-line aligned */
#define SWAP_MAGIC              0x56494245u  /* "VIBE" */

/* Swap slot header written by controller, read by swap_slot PD */
typedef struct __attribute__((packed)) {
    uint32_t    magic;
    uint32_t    version;
    uint32_t    code_format;
    uint32_t    code_offset;
    uint32_t    code_size;
    uint32_t    service_id;
    char        service_name[32];
    uint8_t     _pad[4];
} vibe_slot_header_t;

/* ── Service IDs ────────────────────────────────────────────────────────── */
#define SVC_EVENTBUS    0
#define SVC_MEMFS       1
#define SVC_TOOLSVC     2
#define SVC_MODELSVC    3
#define SVC_AGENTFS     4
#define SVC_LOGSVC      5
#define VS_MAX_SERVICES 8

/* ── Swap states ────────────────────────────────────────────────────────── */
typedef enum {
    SWAP_STATE_IDLE,
    SWAP_STATE_LOADING,
    SWAP_STATE_TESTING,
    SWAP_STATE_ACTIVE,
    SWAP_STATE_ROLLBACK,
} swap_state_t;

typedef struct {
    swap_state_t    state;
    seL4_CPtr       slot_ep;       /* direct endpoint cap — replaces Microkit channel */
    uint32_t        service_id;
    uint32_t        version;
    uint64_t        activated_at;
    uint64_t        health_checks;
    bool            healthy;
} swap_slot_t;

typedef struct {
    const char     *name;
    seL4_CPtr       active_ep;     /* current endpoint serving this service */
    seL4_CPtr       primary_ep;    /* original (static) PD endpoint */
    uint32_t        version;
    bool            swappable;
    uint32_t        rollback_slot;
    bool            has_rollback;
} service_desc_t;

/* ── Module state ───────────────────────────────────────────────────────── */
static swap_slot_t    slots[MAX_SWAP_SLOTS];
static service_desc_t services[VS_MAX_SERVICES];
static int            service_count = 0;
static uint64_t       swap_sequence = 0;

static sel4_server_t  g_srv;

/* Forward declarations */
static int vibe_swap_run_tests(int slot, uint32_t service_id);
int vibe_swap_activate(int slot);

/* ── Nameserver registration ────────────────────────────────────────────── */
static void vs_register_with_nameserver(seL4_CPtr ns_ep)
{
    if (!ns_ep) return;
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = 0xD0u;  /* OP_NS_REGISTER */
    vs_data_wr32(req.data,  0, 0u);
    vs_data_wr32(req.data,  4, 0u);
    vs_data_wr32(req.data,  8, 0u);
    vs_data_wr32(req.data, 12, 1u);
    {
        const char nm[] = "vibe_swap";
        for (int i = 0; nm[i] && (16 + i) < 48; i++)
            req.data[16 + i] = (uint8_t)nm[i];
    }
    req.length = 48;
    sel4_call(ns_ep, &req, &rep);
}

/* ── Initialisation ─────────────────────────────────────────────────────── */
void vibe_swap_init(void)
{
    vs_dbg_puts("[vibe_swap] Initializing swap slot manager\n");

    for (int i = 0; i < MAX_SWAP_SLOTS; i++) {
        slots[i].state      = SWAP_STATE_IDLE;
        slots[i].slot_ep    = 0;   /* filled in by vibe_swap_main */
        slots[i].service_id = 0;
        slots[i].version    = 0;
        slots[i].activated_at  = 0;
        slots[i].health_checks = 0;
        slots[i].healthy       = false;
    }

    services[SVC_EVENTBUS] = (service_desc_t){
        .name = "event_bus", .swappable = false,
        .active_ep = 0, .primary_ep = 0,
        .version = 1, .has_rollback = false,
    };
    services[SVC_MEMFS] = (service_desc_t){
        .name = "memfs", .swappable = true,
        .active_ep = 0, .primary_ep = 0,
        .version = 1, .has_rollback = false,
    };
    services[SVC_TOOLSVC] = (service_desc_t){
        .name = "toolsvc", .swappable = true,
        .active_ep = 0, .primary_ep = 0,
        .version = 1, .has_rollback = false,
    };
    services[SVC_MODELSVC] = (service_desc_t){
        .name = "modelsvc", .swappable = true,
        .active_ep = 0, .primary_ep = 0,
        .version = 1, .has_rollback = false,
    };
    services[SVC_AGENTFS] = (service_desc_t){
        .name = "agentfs", .swappable = true,
        .active_ep = 0, .primary_ep = 0,
        .version = 1, .has_rollback = false,
    };
    service_count = 6;

    vs_dbg_puts("[vibe_swap] Ready for vibe-coded service proposals\n");
}

/* ── Helpers ────────────────────────────────────────────────────────────── */
static int find_free_slot(void) {
    for (int i = 0; i < MAX_SWAP_SLOTS; i++)
        if (slots[i].state == SWAP_STATE_IDLE) return i;
    return -1;
}

static int find_rollback_slot(uint32_t service_id) {
    for (int i = 0; i < MAX_SWAP_SLOTS; i++)
        if (slots[i].state == SWAP_STATE_ROLLBACK &&
            slots[i].service_id == service_id) return i;
    return -1;
}

/* ── vibe_swap_begin ────────────────────────────────────────────────────── */
/*
 * Begin a service swap.
 *
 * Fix for "microkit_ppcall: invalid channel '30'" bug:
 *   We no longer use microkit_notify(slots[slot].channel) where channel
 *   was a stale Microkit integer starting at 30.  Instead we signal
 *   slots[slot].slot_ep — a direct seL4 endpoint cap set at boot.
 */
int vibe_swap_begin(uint32_t service_id, const void *code, uint32_t code_len)
{
    if (service_id >= (uint32_t)VS_MAX_SERVICES) {
        vs_dbg_puts("[vibe_swap] ERROR: invalid service_id\n");
        return -1;
    }

    service_desc_t *svc = &services[service_id];
    if (!svc->swappable) {
        vs_dbg_puts("[vibe_swap] ERROR: service not swappable\n");
        return -2;
    }

    int slot = find_free_slot();
    if (slot < 0) {
        vs_dbg_puts("[vibe_swap] ERROR: no free swap slots\n");
        return -3;
    }

    vs_dbg_puts("[vibe_swap] Beginning swap\n");
    slots[slot].state      = SWAP_STATE_LOADING;
    slots[slot].service_id = service_id;
    slots[slot].version    = svc->version + 1;

    static uintptr_t * const slot_regions[MAX_SWAP_SLOTS] = {
        &swap_code_ctrl_0,
        &swap_code_ctrl_1,
        &swap_code_ctrl_2,
        &swap_code_ctrl_3,
    };
    uintptr_t region_base = *slot_regions[slot];

    if (code_len + SWAP_HEADER_SIZE > SWAP_CODE_REGION_SIZE) {
        vs_dbg_puts("[vibe_swap] ERROR: code too large for slot region\n");
        slots[slot].state = SWAP_STATE_IDLE;
        return -4;
    }

    /* Write slot header — magic set last to commit */
    volatile vibe_slot_header_t *hdr = (volatile vibe_slot_header_t *)region_base;
    hdr->magic       = 0;
    hdr->version     = slots[slot].version;
    hdr->code_format = 1;  /* WASM */
    hdr->code_offset = SWAP_HEADER_SIZE;
    hdr->code_size   = code_len;
    hdr->service_id  = service_id;

    const char *sname = svc->name ? svc->name : "unknown";
    int ni = 0;
    while (sname[ni] && ni < 31) { hdr->service_name[ni] = sname[ni]; ni++; }
    hdr->service_name[ni] = '\0';

    volatile uint8_t *dst = (volatile uint8_t *)(region_base + SWAP_HEADER_SIZE);
    const uint8_t *src = (const uint8_t *)code;
    for (uint32_t i = 0; i < code_len; i++) dst[i] = src[i];

    agentos_wmb();
    hdr->magic = SWAP_MAGIC;
    agentos_wmb();

    vs_dbg_puts("[vibe_swap] WASM image written, signalling slot\n");

    /*
     * Signal the swap slot to begin loading.
     * Fix: use direct endpoint cap instead of stale Microkit channel integer.
     */
    if (slots[slot].slot_ep) seL4_Signal(slots[slot].slot_ep);

    if (vibe_swap_run_tests(slot, service_id) != 0) {
        vs_dbg_puts("[vibe_swap] Conformance tests FAILED — slot not activated\n");
        return -5;
    }

    vs_dbg_puts("[vibe_swap] Conformance tests passed — activating slot\n");

    if (vibe_swap_activate(slot) != 0) {
        vs_dbg_puts("[vibe_swap] Activation failed after successful tests\n");
        slots[slot].state = SWAP_STATE_IDLE;
        return -6;
    }

    return slot;
}

/* ── vibe_swap_run_tests ────────────────────────────────────────────────── */
/*
 * Run conformance tests on a freshly-loaded swap slot.
 *
 * Fix: PPCALL_DONATE(slots[slot].channel, ...) replaced with
 *      sel4_call(slots[slot].slot_ep, &req, &rep).
 */
static int vibe_swap_run_tests(int slot, uint32_t service_id)
{
    slots[slot].state = SWAP_STATE_TESTING;

    if (service_id == (uint32_t)SVC_MEMFS) {
        static const char test_key[] = "test/key";
        static const char test_val[] = "testval";

        if (slots[slot].slot_ep) {
            sel4_msg_t req = {0}, rep = {0};
            req.opcode = STORAGE_OP_WRITE;
            vs_data_wr32(req.data, 0, (uint32_t)(uintptr_t)test_key);
            vs_data_wr32(req.data, 4, 8u);
            vs_data_wr32(req.data, 8, (uint32_t)(uintptr_t)test_val);
            vs_data_wr32(req.data, 12, 7u);
            req.length = 16;
            sel4_call(slots[slot].slot_ep, &req, &rep);
            uint32_t write_status = vs_data_rd32(rep.data, 0);
            if (write_status != 0) {
                vs_dbg_puts("[vibe_swap] Testing slot: write... FAIL\n");
                slots[slot].state = SWAP_STATE_IDLE;
                return -1;
            }
            vs_dbg_puts("[vibe_swap] Testing slot: write... PASS\n");

            sel4_msg_t rreq = {0}, rrep = {0};
            rreq.opcode = STORAGE_OP_READ;
            vs_data_wr32(rreq.data, 0, (uint32_t)(uintptr_t)test_key);
            vs_data_wr32(rreq.data, 4, 8u);
            rreq.length = 8;
            sel4_call(slots[slot].slot_ep, &rreq, &rrep);
            uint32_t read_ptr = vs_data_rd32(rrep.data, 0);
            if (read_ptr == 0) {
                vs_dbg_puts("[vibe_swap] Testing slot: read... FAIL\n");
                slots[slot].state = SWAP_STATE_IDLE;
                return -1;
            }
            vs_dbg_puts("[vibe_swap] Testing slot: read... PASS\n");
        }
    }

    /* Health check — all services */
    if (slots[slot].slot_ep) {
        sel4_msg_t req = {0}, rep = {0};
        req.opcode = AOS_LABEL_HEALTH;
        req.length = 0;
        sel4_call(slots[slot].slot_ep, &req, &rep);
        uint32_t health_status = vs_data_rd32(rep.data, 0);
        if (health_status != 0) {
            vs_dbg_puts("[vibe_swap] Testing slot: health... FAIL\n");
            slots[slot].state = SWAP_STATE_IDLE;
            return -1;
        }
        vs_dbg_puts("[vibe_swap] Testing slot: health... PASS\n");
    } else {
        /* No endpoint — skip test in host/stub environment */
        vs_dbg_puts("[vibe_swap] Testing slot: health... SKIP (no ep)\n");
    }

    return 0;
}

/* ── vibe_swap_activate ─────────────────────────────────────────────────── */
int vibe_swap_activate(int slot)
{
    if (slot < 0 || slot >= MAX_SWAP_SLOTS) return -1;
    if (slots[slot].state != SWAP_STATE_TESTING) return -2;

    uint32_t svc_id = slots[slot].service_id;
    service_desc_t *svc = &services[svc_id];

    vs_dbg_puts("[vibe_swap] Activating slot\n");

    int old_rollback = find_rollback_slot(svc_id);
    if (old_rollback >= 0)
        slots[old_rollback].state = SWAP_STATE_IDLE;

    for (int i = 0; i < MAX_SWAP_SLOTS; i++) {
        if (slots[i].state == SWAP_STATE_ACTIVE &&
            slots[i].service_id == svc_id) {
            slots[i].state = SWAP_STATE_ROLLBACK;
            svc->rollback_slot = i;
            svc->has_rollback = true;
            break;
        }
    }

    slots[slot].state      = SWAP_STATE_ACTIVE;
    slots[slot].activated_at  = swap_sequence++;
    slots[slot].health_checks = 0;
    slots[slot].healthy       = true;

    /* Route service to the new slot's endpoint */
    svc->active_ep = slots[slot].slot_ep;
    svc->version   = slots[slot].version;

    vs_dbg_puts("[vibe_swap] *** SERVICE SWAPPED ***\n");
    return 0;
}

/* ── vibe_swap_rollback ─────────────────────────────────────────────────── */
int vibe_swap_rollback(uint32_t service_id)
{
    if (service_id >= (uint32_t)VS_MAX_SERVICES) return -1;

    for (int i = 0; i < MAX_SWAP_SLOTS; i++) {
        if (slots[i].state == SWAP_STATE_TESTING &&
            slots[i].service_id == service_id) {
            vs_dbg_puts("[vibe_swap] rollback: slot still in TESTING — reset to IDLE\n");
            slots[i].state = SWAP_STATE_IDLE;
            return 0;
        }
    }

    service_desc_t *svc = &services[service_id];
    if (!svc->has_rollback) {
        vs_dbg_puts("[vibe_swap] ERROR: no rollback available\n");
        return -2;
    }

    int rb_slot = (int)svc->rollback_slot;
    vs_dbg_puts("[vibe_swap] Rolling back service\n");

    for (int i = 0; i < MAX_SWAP_SLOTS; i++) {
        if (slots[i].state == SWAP_STATE_ACTIVE &&
            slots[i].service_id == service_id) {
            slots[i].state = SWAP_STATE_IDLE;
            break;
        }
    }

    slots[rb_slot].state = SWAP_STATE_ACTIVE;
    svc->active_ep   = slots[rb_slot].slot_ep;
    svc->version     = slots[rb_slot].version;
    svc->has_rollback = false;

    vs_dbg_puts("[vibe_swap] Rollback complete\n");
    return 0;
}

/* ── vibe_swap_health_check ─────────────────────────────────────────────── */
bool vibe_swap_health_check(int slot)
{
    if (slot < 0 || slot >= MAX_SWAP_SLOTS) return false;
    if (slots[slot].state != SWAP_STATE_ACTIVE) return false;
    if (!slots[slot].slot_ep) return false;

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = AOS_LABEL_HEALTH;
    req.length = 0;
    sel4_call(slots[slot].slot_ep, &req, &rep);

    uint32_t status = vs_data_rd32(rep.data, 0);
    bool healthy = (status == 0);

    slots[slot].healthy = healthy;
    if (healthy) slots[slot].health_checks++;
    return healthy;
}

/* ── vibe_swap_status ───────────────────────────────────────────────────── */
void vibe_swap_status(uint32_t service_id, uint32_t *version,
                       seL4_CPtr *active_ep, bool *has_rollback)
{
    if (service_id >= (uint32_t)VS_MAX_SERVICES) return;
    service_desc_t *svc = &services[service_id];
    if (version)     *version     = svc->version;
    if (active_ep)   *active_ep   = svc->active_ep;
    if (has_rollback) *has_rollback = svc->has_rollback;
}

/* ── vibe_swap_health_notify ────────────────────────────────────────────── */
int vibe_swap_health_notify(int slot)
{
    if (slot < 0 || slot >= MAX_SWAP_SLOTS) return -1;
    if (slots[slot].state != SWAP_STATE_TESTING) {
        vs_dbg_puts("[vibe_swap] health_notify: slot not in TESTING state\n");
        return -2;
    }
    slots[slot].healthy = true;
    return vibe_swap_activate(slot);
}

/* ── IPC Handlers ───────────────────────────────────────────────────────── */

static uint32_t handle_swap_health(sel4_badge_t badge, const sel4_msg_t *req,
                                    sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    uint32_t slot_id = vs_data_rd32(req->data, 0);
    bool healthy = vibe_swap_health_check((int)slot_id);
    vs_data_wr32(rep->data, 0, healthy ? 0u : 1u);
    vs_data_wr32(rep->data, 4, (uint32_t)(slots[slot_id < MAX_SWAP_SLOTS ? slot_id : 0].state));
    rep->length = 8;
    return SEL4_ERR_OK;
}

static uint32_t handle_swap_status_op(sel4_badge_t badge, const sel4_msg_t *req,
                                       sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;
    uint32_t in_flight = 0;
    for (int i = 0; i < MAX_SWAP_SLOTS; i++) {
        if (slots[i].state == SWAP_STATE_LOADING ||
            slots[i].state == SWAP_STATE_TESTING) in_flight++;
    }
    vs_data_wr32(rep->data, 0, in_flight);
    vs_data_wr32(rep->data, 4, (uint32_t)swap_sequence);
    vs_data_wr32(rep->data, 8, 0u);  /* rollbacks: tracked in service table */
    rep->length = 12;
    return SEL4_ERR_OK;
}

/* ── Test-only shim ─────────────────────────────────────────────────────── */
#ifdef AGENTOS_TEST_HOST
static void vibe_swap_test_init(void)
{
    vibe_swap_init();
}

static uint32_t vibe_swap_dispatch_one(sel4_badge_t badge,
                                        const sel4_msg_t *req,
                                        sel4_msg_t *rep)
{
    switch (req->opcode) {
    case MSG_VIBE_SWAP_HEALTH:  return handle_swap_health(badge, req, rep, (void*)0);
    case MSG_VIBE_SWAP_STATUS:  return handle_swap_status_op(badge, req, rep, (void*)0);
    default:
        rep->opcode = SEL4_ERR_INVALID_OP;
        rep->length = 0;
        return SEL4_ERR_INVALID_OP;
    }
}
#endif

/* ── Entry point ────────────────────────────────────────────────────────── */
#ifndef AGENTOS_TEST_HOST
/*
 * vibe_swap_main — called by root-task boot dispatcher.
 *
 * Parameters:
 *   my_ep        — this PD's receive endpoint
 *   ns_ep        — nameserver endpoint
 *   slot_eps[]   — direct endpoint caps for each swap_slot PD (indexed 0..3)
 *                  Replaces stale Microkit channel IDs 30-33.
 */
void vibe_swap_main(seL4_CPtr my_ep, seL4_CPtr ns_ep,
                    seL4_CPtr slot_ep0, seL4_CPtr slot_ep1,
                    seL4_CPtr slot_ep2, seL4_CPtr slot_ep3)
{
    vibe_swap_init();

    /* Populate slot endpoint caps — fixes invalid-channel-30 bug */
    slots[0].slot_ep = slot_ep0;
    slots[1].slot_ep = slot_ep1;
    slots[2].slot_ep = slot_ep2;
    slots[3].slot_ep = slot_ep3;

    vs_register_with_nameserver(ns_ep);

    sel4_server_init(&g_srv, my_ep);
    sel4_server_register(&g_srv, MSG_VIBE_SWAP_HEALTH,  handle_swap_health,     (void*)0);
    sel4_server_register(&g_srv, MSG_VIBE_SWAP_STATUS,  handle_swap_status_op,  (void*)0);

    vs_dbg_puts("[vibe_swap] *** VibeSwap ALIVE ***\n");
    sel4_server_run(&g_srv);  /* NEVER RETURNS */
}
#endif /* !AGENTOS_TEST_HOST */
