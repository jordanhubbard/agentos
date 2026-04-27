/*
 * agentOS VibeEngine Protection Domain — E5-S6: raw seL4 IPC
 *
 * The VibeEngine is the userspace service that manages the hot-swap
 * lifecycle: agents submit WASM proposals, VibeEngine validates them,
 * and triggers the kernel-side vibe_swap pipeline via the controller.
 *
 * This closes the end-to-end loop:
 *   Agent → VibeEngine (propose/validate/execute) → Controller →
 *   vibe_swap_begin → swap_slot loads WASM via wasm3 → health check →
 *   service live
 *
 * Migration from Microkit:
 *   Old: CH_SERIAL_PD (44) and CH_BLOCK_PD (49) were stale Microkit
 *        channel IDs — using them caused "microkit_ppcall: invalid channel"
 *        errors.  Fixed by replacing with nameserver-resolved endpoint caps.
 *   Old: CH_CTRL (1) was a Microkit notify channel.
 *        Fixed by passing ctrl_ep directly from root task at boot.
 *
 * Entry point: void vibe_engine_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* ── Conditional compilation ───────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST
/*
 * Host-side test build: provide minimal type stubs so this file compiles
 * without seL4 or Microkit headers.  The test file provides framework.h
 * (which defines microkit_mr_set/get) before including this unit.
 *
 * The guard AGENTOS_SEL4_STUBS_DEFINED prevents duplicate definitions when
 * multiple source files are included into a single test translation unit.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

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

/* In test builds sel4_call and seL4_Signal are no-ops */
static inline void sel4_call(seL4_CPtr ep, const sel4_msg_t *req, sel4_msg_t *rep)
{
    (void)ep; (void)req;
    rep->opcode = 0;
    rep->length = 0;
}
static inline void seL4_Signal(seL4_CPtr cap) { (void)cap; }
static inline void seL4_DebugPutChar(char c)  { (void)c; }

/* microkit stubs used by ns_pack_name / agentos_wmb */
static inline void microkit_mr_set(uint32_t i, uint64_t v) { (void)i; (void)v; }
static inline uint64_t microkit_mr_get(uint32_t i) { (void)i; return 0; }

#endif /* AGENTOS_SEL4_STUBS_DEFINED */

#else  /* !AGENTOS_TEST_HOST */

#include <stdbool.h>
#include "sel4_ipc.h"
#include "sel4_server.h"
#include "sel4_client.h"

#endif /* AGENTOS_TEST_HOST */

/* ── Opcode definitions (guard against double-include from agentos.h) ──────── */

#ifndef OP_VIBE_PROPOSE
#define OP_VIBE_PROPOSE           0x40u
#define OP_VIBE_VALIDATE          0x41u
#define OP_VIBE_EXECUTE           0x42u
#define OP_VIBE_STATUS            0x43u
#define OP_VIBE_ROLLBACK          0x44u
#define OP_VIBE_HEALTH            0x45u
#define OP_VIBE_REGISTER_SERVICE  0x46u
#define OP_VIBE_LIST_SERVICES     0x47u
#endif

/* VibeOS lifecycle opcodes */
#ifndef MSG_VIBEOS_CREATE
#define MSG_VIBEOS_CREATE               0x2401u
#define MSG_VIBEOS_DESTROY              0x2402u
#define MSG_VIBEOS_STATUS               0x2403u
#define MSG_VIBEOS_LIST                 0x2404u
#define MSG_VIBEOS_BIND_DEVICE          0x2405u
#define MSG_VIBEOS_UNBIND_DEVICE        0x2406u
#define MSG_VIBEOS_SNAPSHOT             0x2407u
#define MSG_VIBEOS_RESTORE              0x2408u
#define MSG_VIBEOS_MIGRATE              0x2409u
#define MSG_VIBEOS_BOOT                 0x240Au
#define MSG_VIBEOS_LOAD_MODULE          0x240Bu
#define MSG_VIBEOS_CHECK_SERVICE_EXISTS 0x240Cu
#define MSG_VIBEOS_CONFIGURE            0x240Du
#endif

/* VibeOS error codes */
#ifndef VIBEOS_OK
#define VIBEOS_OK                  0u
#define VIBEOS_ERR_BAD_TYPE        1u
#define VIBEOS_ERR_OOM             2u
#define VIBEOS_ERR_BAD_HANDLE      3u
#define VIBEOS_ERR_NO_HANDLE       4u
#define VIBEOS_ERR_WRONG_STATE     5u
#define VIBEOS_ERR_NOT_IMPL        6u
#define VIBEOS_ERR_BAD_MODULE_TYPE 7u
#define VIBEOS_ERR_WASM_LOAD_FAIL  8u
#define VIBEOS_ERR_BAD_FUNC_CLASS  9u
#endif

/* VibeOS states */
#ifndef VIBEOS_STATE_CREATING
#define VIBEOS_STATE_CREATING  0u
#define VIBEOS_STATE_BOOTING   1u
#define VIBEOS_STATE_RUNNING   2u
#define VIBEOS_STATE_PAUSED    3u
#define VIBEOS_STATE_SNAPSHOT  4u
#define VIBEOS_STATE_DEAD      5u
#endif

/* VibeOS device flags */
#ifndef VIBEOS_DEV_SERIAL
#define VIBEOS_DEV_SERIAL   (1u << 0)
#define VIBEOS_DEV_NET      (1u << 1)
#define VIBEOS_DEV_BLOCK    (1u << 2)
#endif

/* VibeOS module types */
#ifndef VIBEOS_MODULE_TYPE_WASM
#define VIBEOS_MODULE_TYPE_WASM  1u
#define VIBEOS_MODULE_TYPE_ELF   2u
#endif

/* VM manager opcodes */
#ifndef OP_VM_CREATE
#define OP_VM_CREATE      0x3001u
#define OP_VM_START       0x3002u
#define OP_VM_STOP        0x3003u
#define OP_VM_DESTROY     0x3004u
#define OP_VM_INFO        0x3005u
#define OP_VM_SNAPSHOT    0x3006u
#define OP_VM_RESTORE     0x3007u
#define OP_VM_CONFIGURE   0x3008u
#endif

/* Serial / block open opcodes */
#ifndef MSG_SERIAL_OPEN
#define MSG_SERIAL_OPEN  0x2001u
#define MSG_BLOCK_OPEN   0x2201u
#define MSG_NET_OPEN     0x2101u
#endif

/* ── Error / result codes ────────────────────────────────────────────────── */
#define VIBE_OK             0u
#define VIBE_ERR_FULL       1u
#define VIBE_ERR_BADWASM    2u
#define VIBE_ERR_TOOBIG     3u
#define VIBE_ERR_NOSVC      4u
#define VIBE_ERR_NOENT      5u
#define VIBE_ERR_STATE      6u
#define VIBE_ERR_VALFAIL    7u
#define VIBE_ERR_INTERNAL   99u

/* ── WASM magic bytes ───────────────────────────────────────────────────── */
#define WASM_MAGIC_0  0x00u
#define WASM_MAGIC_1  0x61u   /* 'a' */
#define WASM_MAGIC_2  0x73u   /* 's' */
#define WASM_MAGIC_3  0x6Du   /* 'm' */

/* ── data[] byte-offset helpers (no-libc, no seL4 dep) ─────────────────── */
static inline uint32_t data_rd32(const uint8_t *d, int off) {
    return (uint32_t)d[off]         |
           ((uint32_t)d[off+1] << 8)  |
           ((uint32_t)d[off+2] << 16) |
           ((uint32_t)d[off+3] << 24);
}
static inline void data_wr32(uint8_t *d, int off, uint32_t v) {
    d[off]   = (uint8_t)(v & 0xFFu);
    d[off+1] = (uint8_t)((v >>  8) & 0xFFu);
    d[off+2] = (uint8_t)((v >> 16) & 0xFFu);
    d[off+3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ── Debug output helper ────────────────────────────────────────────────── */
static void dbg_puts(const char *s) {
#ifdef CONFIG_PRINTING
    for (; *s; s++) seL4_DebugPutChar(*s);
#else
    (void)s;
#endif
}

/* ── Memory barrier ─────────────────────────────────────────────────────── */
#ifndef agentos_wmb
#ifdef AGENTOS_TEST_HOST
#define agentos_wmb() ((void)0)
#else
#define agentos_wmb() __asm__ volatile("dmb st" ::: "memory")
#endif
#endif

/* ── Staging region ─────────────────────────────────────────────────────── */
#define STAGING_SIZE      0x400000UL  /* 4MB staging region */
#define MAX_WASM_SIZE     (STAGING_SIZE - 64)

/* vibe_staging_vaddr is set by root-task boot dispatcher before calling main */
static uintptr_t vibe_staging_vaddr;

/* ── Proposal management ────────────────────────────────────────────────── */
#define MAX_PROPOSALS     8
#define MAX_SERVICES      8

typedef enum {
    PROP_STATE_FREE,
    PROP_STATE_PENDING,
    PROP_STATE_VALIDATED,
    PROP_STATE_APPROVED,
    PROP_STATE_ACTIVE,
    PROP_STATE_REJECTED,
    PROP_STATE_ROLLEDBACK,
} proposal_state_t;

typedef struct {
    proposal_state_t state;
    uint32_t         service_id;
    uint32_t         wasm_offset;
    uint32_t         wasm_size;
    uint32_t         cap_tag;
    uint32_t         version;
    uint32_t         val_checks;
    bool             val_passed;
} proposal_t;

typedef struct {
    const char *name;
    bool        swappable;
    uint32_t    current_version;
    uint32_t    max_wasm_bytes;
} service_entry_t;

/* ── VOS instance table ─────────────────────────────────────────────────── */
#define MAX_VOS_INSTANCES  4

typedef struct {
    bool     active;
    uint32_t handle;
    uint32_t vm_slot;
    uint8_t  os_type;
    uint8_t  state;
    uint32_t ram_mb;
    uint32_t dev_mask;
    uint32_t dev_handles[5];
    uint8_t  module_hash[32];
    uint32_t swap_id;
} vos_instance_t;

/* ── Module state ───────────────────────────────────────────────────────── */
static proposal_t     proposals[MAX_PROPOSALS];
static service_entry_t services[MAX_SERVICES];
static uint32_t       service_count = 0;
static uint32_t       next_proposal_id = 1;
static uint64_t       total_proposals = 0;
static uint64_t       total_swaps = 0;
static uint64_t       total_rejections = 0;
static vos_instance_t s_vos[MAX_VOS_INSTANCES];
static uint32_t       s_next_handle = 1;

/* Endpoint caps resolved at boot — no more stale channel IDs */
static seL4_CPtr g_ctrl_ep   = 0;  /* controller endpoint (was CH_CTRL=1)         */
static seL4_CPtr g_vmm_ep    = 0;  /* vm_manager endpoint (was CH_VMM)             */
static seL4_CPtr g_serial_ep = 0;  /* serial_pd endpoint (was stale CH_SERIAL_PD=44) */
static seL4_CPtr g_block_ep  = 0;  /* block_pd endpoint (was stale CH_BLOCK_PD=49)   */
static seL4_CPtr g_net_ep    = 0;  /* net_pd endpoint (was CH_NET_PD=48)             */

static sel4_server_t g_srv;

/* Forward declarations */
static bool validate_wasm_header(const uint8_t *data, uint32_t size);
static int  vos_find(uint32_t handle);
static int  vos_alloc(void);

/* ── Nameserver registration ────────────────────────────────────────────── */
static void register_with_nameserver(seL4_CPtr ns_ep)
{
    if (!ns_ep) return;
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = 0xD0u;  /* OP_NS_REGISTER */
    /* data[0..3]  = channel_id (0 for dynamic PD) */
    /* data[4..7]  = pd_id (0 for vibe_engine) */
    /* data[8..11] = cap_classes (0) */
    /* data[12..15]= version (1) */
    /* data[16..47]= name "vibe_engine\0" */
    data_wr32(req.data,  0, 0u);
    data_wr32(req.data,  4, 0u);
    data_wr32(req.data,  8, 0u);
    data_wr32(req.data, 12, 1u);
    {
        const char nm[] = "vibe_engine";
        for (int i = 0; nm[i] && (16 + i) < 48; i++)
            req.data[16 + i] = (uint8_t)nm[i];
    }
    req.length = 48;
    sel4_call(ns_ep, &req, &rep);
}

/* ── Nameserver lookup helper ───────────────────────────────────────────── */
static seL4_CPtr lookup_service(seL4_CPtr ns_ep, const char *name)
{
    if (!ns_ep) return 0;
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = 0xD1u;  /* OP_NS_LOOKUP */
    for (int i = 0; name[i] && i < 47; i++)
        req.data[i] = (uint8_t)name[i];
    req.length = 47;
    sel4_call(ns_ep, &req, &rep);
    if (rep.opcode == 0u)  /* NS_OK */
        return (seL4_CPtr)data_rd32(rep.data, 0);  /* channel_id field */
    return 0;
}

/* ── Helpers ────────────────────────────────────────────────────────────── */
static int find_free_proposal(void) {
    for (int i = 0; i < MAX_PROPOSALS; i++)
        if (proposals[i].state == PROP_STATE_FREE) return i;
    return -1;
}

static bool validate_wasm_header(const uint8_t *data, uint32_t size) {
    if (size < 8) return false;
    return (data[0] == WASM_MAGIC_0 &&
            data[1] == WASM_MAGIC_1 &&
            data[2] == WASM_MAGIC_2 &&
            data[3] == WASM_MAGIC_3);
}

static int vos_find(uint32_t handle) {
    for (int i = 0; i < MAX_VOS_INSTANCES; i++)
        if (s_vos[i].active && s_vos[i].handle == handle) return i;
    return -1;
}

static int vos_alloc(void) {
    for (int i = 0; i < MAX_VOS_INSTANCES; i++)
        if (!s_vos[i].active) return i;
    return -1;
}

static void register_services(void) {
    services[0] = (service_entry_t){
        .name = "event_bus", .swappable = false,
        .current_version = 1, .max_wasm_bytes = 0,
    };
    services[1] = (service_entry_t){
        .name = "memfs", .swappable = true,
        .current_version = 1, .max_wasm_bytes = 2 * 1024 * 1024,
    };
    services[2] = (service_entry_t){
        .name = "toolsvc", .swappable = true,
        .current_version = 1, .max_wasm_bytes = 2 * 1024 * 1024,
    };
    services[3] = (service_entry_t){
        .name = "modelsvc", .swappable = true,
        .current_version = 1, .max_wasm_bytes = 4 * 1024 * 1024,
    };
    services[4] = (service_entry_t){
        .name = "agentfs", .swappable = true,
        .current_version = 1, .max_wasm_bytes = 2 * 1024 * 1024,
    };
    services[5] = (service_entry_t){
        .name = "logsvc", .swappable = true,
        .current_version = 1, .max_wasm_bytes = 1 * 1024 * 1024,
    };
    service_count = 6;
}

/* ── Handler: OP_VIBE_PROPOSE ───────────────────────────────────────────── */
static uint32_t handle_propose(sel4_badge_t badge, const sel4_msg_t *req,
                                sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    uint32_t service_id = data_rd32(req->data, 0);
    uint32_t wasm_size  = data_rd32(req->data, 4);
    uint32_t cap_tag    = data_rd32(req->data, 8);

    if (service_id >= service_count) {
        dbg_puts("[vibe_engine] REJECT: unknown service\n");
        data_wr32(rep->data, 0, VIBE_ERR_NOSVC);
        rep->length = 4;
        return VIBE_ERR_NOSVC;
    }
    if (!services[service_id].swappable) {
        dbg_puts("[vibe_engine] REJECT: service not swappable\n");
        data_wr32(rep->data, 0, VIBE_ERR_NOSVC);
        rep->length = 4;
        return VIBE_ERR_NOSVC;
    }
    if (wasm_size > MAX_WASM_SIZE ||
        wasm_size > services[service_id].max_wasm_bytes) {
        dbg_puts("[vibe_engine] REJECT: WASM too large\n");
        data_wr32(rep->data, 0, VIBE_ERR_TOOBIG);
        rep->length = 4;
        return VIBE_ERR_TOOBIG;
    }

    const uint8_t *staged = (const uint8_t *)vibe_staging_vaddr;
    if (!validate_wasm_header(staged, wasm_size)) {
        dbg_puts("[vibe_engine] REJECT: bad WASM magic\n");
        data_wr32(rep->data, 0, VIBE_ERR_BADWASM);
        rep->length = 4;
        return VIBE_ERR_BADWASM;
    }

    int slot = find_free_proposal();
    if (slot < 0) {
        dbg_puts("[vibe_engine] REJECT: proposal table full\n");
        data_wr32(rep->data, 0, VIBE_ERR_FULL);
        rep->length = 4;
        return VIBE_ERR_FULL;
    }

    proposals[slot].state       = PROP_STATE_PENDING;
    proposals[slot].service_id  = service_id;
    proposals[slot].wasm_offset = 0;
    proposals[slot].wasm_size   = wasm_size;
    proposals[slot].cap_tag     = cap_tag;
    proposals[slot].version     = next_proposal_id++;
    proposals[slot].val_checks  = 0;
    proposals[slot].val_passed  = false;
    total_proposals++;

    dbg_puts("[vibe_engine] Proposal accepted\n");
    data_wr32(rep->data, 0, VIBE_OK);
    data_wr32(rep->data, 4, proposals[slot].version);
    rep->length = 8;
    return SEL4_ERR_OK;
}

/* ── Handler: OP_VIBE_VALIDATE ─────────────────────────────────────────── */
static uint32_t handle_validate(sel4_badge_t badge, const sel4_msg_t *req,
                                 sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    uint32_t proposal_id = data_rd32(req->data, 0);

    int slot = -1;
    for (int i = 0; i < MAX_PROPOSALS; i++) {
        if (proposals[i].state != PROP_STATE_FREE &&
            proposals[i].version == proposal_id) { slot = i; break; }
    }
    if (slot < 0) {
        data_wr32(rep->data, 0, VIBE_ERR_NOENT);
        rep->length = 4;
        return VIBE_ERR_NOENT;
    }
    if (proposals[slot].state != PROP_STATE_PENDING) {
        data_wr32(rep->data, 0, VIBE_ERR_STATE);
        rep->length = 4;
        return VIBE_ERR_STATE;
    }

    uint32_t checks = 0;
    bool all_pass = true;

    const uint8_t *staged = (const uint8_t *)(vibe_staging_vaddr + proposals[slot].wasm_offset);
    if (validate_wasm_header(staged, proposals[slot].wasm_size))
        checks |= (1u << 0);
    else
        all_pass = false;

    uint32_t svc_id = proposals[slot].service_id;
    if (proposals[slot].wasm_size <= services[svc_id].max_wasm_bytes)
        checks |= (1u << 1);
    else
        all_pass = false;

    if (services[svc_id].swappable)
        checks |= (1u << 2);
    else
        all_pass = false;

    if (proposals[slot].cap_tag != 0)
        checks |= (1u << 3);
    else
        all_pass = false;

    proposals[slot].val_checks = checks;
    proposals[slot].val_passed = all_pass;

    if (all_pass) {
        proposals[slot].state = PROP_STATE_VALIDATED;
        dbg_puts("[vibe_engine] Validation PASSED\n");
    } else {
        proposals[slot].state = PROP_STATE_REJECTED;
        total_rejections++;
        dbg_puts("[vibe_engine] Validation FAILED\n");
    }

    data_wr32(rep->data, 0, all_pass ? VIBE_OK : VIBE_ERR_VALFAIL);
    data_wr32(rep->data, 4, checks);
    rep->length = 8;
    return SEL4_ERR_OK;
}

/* ── Handler: OP_VIBE_EXECUTE ───────────────────────────────────────────── */
/*
 * Fix for bug: "microkit_ppcall: invalid channel '30'" and '44'/'49':
 *   - Controller is now notified via seL4_Signal(g_ctrl_ep) — a direct
 *     endpoint cap distributed by root task at boot, not a channel number.
 *   - g_serial_ep and g_block_ep are nameserver-resolved, not stale IDs.
 */
static uint32_t handle_execute(sel4_badge_t badge, const sel4_msg_t *req,
                                sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    uint32_t proposal_id = data_rd32(req->data, 0);

    int slot = -1;
    for (int i = 0; i < MAX_PROPOSALS; i++) {
        if (proposals[i].state != PROP_STATE_FREE &&
            proposals[i].version == proposal_id) { slot = i; break; }
    }
    if (slot < 0) {
        data_wr32(rep->data, 0, VIBE_ERR_NOENT);
        rep->length = 4;
        return VIBE_ERR_NOENT;
    }
    if (proposals[slot].state != PROP_STATE_VALIDATED) {
        data_wr32(rep->data, 0, VIBE_ERR_STATE);
        rep->length = 4;
        return VIBE_ERR_STATE;
    }

    proposals[slot].state = PROP_STATE_APPROVED;

    /* Write swap metadata at end of staging region (avoids clobbering WASM) */
    volatile uint8_t *meta = (volatile uint8_t *)(vibe_staging_vaddr + STAGING_SIZE - 64);
    uint32_t svc = proposals[slot].service_id;
    uint32_t off = proposals[slot].wasm_offset;
    uint32_t sz  = proposals[slot].wasm_size;
    uint32_t pid = proposals[slot].version;

    meta[0]  = svc & 0xff; meta[1]  = (svc >> 8) & 0xff;
    meta[2]  = (svc >> 16) & 0xff; meta[3]  = (svc >> 24) & 0xff;
    meta[4]  = off & 0xff; meta[5]  = (off >> 8) & 0xff;
    meta[6]  = (off >> 16) & 0xff; meta[7]  = (off >> 24) & 0xff;
    meta[8]  = sz & 0xff;  meta[9]  = (sz >> 8) & 0xff;
    meta[10] = (sz >> 16) & 0xff;  meta[11] = (sz >> 24) & 0xff;
    meta[12] = pid & 0xff; meta[13] = (pid >> 8) & 0xff;
    meta[14] = (pid >> 16) & 0xff; meta[15] = (pid >> 24) & 0xff;

    agentos_wmb();

    dbg_puts("[vibe_engine] *** SWAP APPROVED — signalling controller ***\n");

    /*
     * Signal the controller via its direct endpoint cap.
     * This replaces microkit_notify(CH_CTRL) which used stale channel 1.
     */
    if (g_ctrl_ep) seL4_Signal(g_ctrl_ep);

    total_swaps++;
    proposals[slot].state = PROP_STATE_ACTIVE;
    services[proposals[slot].service_id].current_version++;

    data_wr32(rep->data, 0, VIBE_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/* ── Handler: OP_VIBE_STATUS ────────────────────────────────────────────── */
static uint32_t handle_status(sel4_badge_t badge, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    uint32_t proposal_id = data_rd32(req->data, 0);

    data_wr32(rep->data,  0, VIBE_OK);
    data_wr32(rep->data,  4, (uint32_t)total_proposals);
    data_wr32(rep->data,  8, (uint32_t)total_swaps);
    data_wr32(rep->data, 12, (uint32_t)total_rejections);

    if (proposal_id > 0) {
        for (int i = 0; i < MAX_PROPOSALS; i++) {
            if (proposals[i].version == proposal_id) {
                data_wr32(rep->data, 16, (uint32_t)proposals[i].state);
                rep->length = 20;
                return SEL4_ERR_OK;
            }
        }
        data_wr32(rep->data, 16, 0xFFFFFFFFu);
        rep->length = 20;
        return SEL4_ERR_OK;
    }

    rep->length = 16;
    return SEL4_ERR_OK;
}

/* ── Handler: OP_VIBE_ROLLBACK ──────────────────────────────────────────── */
static uint32_t handle_rollback(sel4_badge_t badge, const sel4_msg_t *req,
                                 sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    uint32_t service_id = data_rd32(req->data, 0);

    if (service_id >= service_count || !services[service_id].swappable) {
        data_wr32(rep->data, 0, VIBE_ERR_NOSVC);
        rep->length = 4;
        return VIBE_ERR_NOSVC;
    }

    volatile uint8_t *meta = (volatile uint8_t *)(vibe_staging_vaddr + STAGING_SIZE - 64);
    uint32_t svc = service_id;
    meta[0]  = svc & 0xff; meta[1]  = (svc >> 8) & 0xff;
    meta[2]  = (svc >> 16) & 0xff; meta[3]  = (svc >> 24) & 0xff;
    meta[4]  = 0; meta[5] = 0; meta[6] = 0; meta[7] = 0;
    meta[8]  = 0xFF; meta[9] = 0xFF; meta[10] = 0xFF; meta[11] = 0xFF;

    agentos_wmb();
    if (g_ctrl_ep) seL4_Signal(g_ctrl_ep);

    for (int i = 0; i < MAX_PROPOSALS; i++) {
        if (proposals[i].state == PROP_STATE_ACTIVE &&
            proposals[i].service_id == service_id) {
            proposals[i].state = PROP_STATE_ROLLEDBACK;
            break;
        }
    }

    data_wr32(rep->data, 0, VIBE_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/* ── Handler: OP_VIBE_HEALTH ────────────────────────────────────────────── */
static uint32_t handle_health(sel4_badge_t badge, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;
    data_wr32(rep->data, 0, VIBE_OK);
    data_wr32(rep->data, 4, (uint32_t)total_proposals);
    data_wr32(rep->data, 8, (uint32_t)total_swaps);
    rep->length = 12;
    return SEL4_ERR_OK;
}

/* ── Handler: OP_VIBE_REGISTER_SERVICE ─────────────────────────────────── */
static uint32_t handle_register_service(sel4_badge_t badge, const sel4_msg_t *req,
                                         sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    uint32_t name_ptr = data_rd32(req->data,  0);
    uint32_t name_len = data_rd32(req->data,  4);
    uint32_t max_wasm = data_rd32(req->data,  8);

    if (name_len == 0 || name_len > 31) {
        data_wr32(rep->data, 0, VIBE_ERR_INTERNAL);
        rep->length = 4;
        return VIBE_ERR_INTERNAL;
    }
    if (service_count >= MAX_SERVICES) {
        data_wr32(rep->data, 0, VIBE_ERR_FULL);
        rep->length = 4;
        return VIBE_ERR_FULL;
    }
    if (name_ptr + name_len > STAGING_SIZE - 64) {
        data_wr32(rep->data, 0, VIBE_ERR_INTERNAL);
        rep->length = 4;
        return VIBE_ERR_INTERNAL;
    }

    const char *src_name = (const char *)(vibe_staging_vaddr + name_ptr);

    for (uint32_t i = 0; i < service_count; i++) {
        if (!services[i].name) continue;
        const char *existing = services[i].name;
        bool match = true;
        uint32_t j = 0;
        for (; j < name_len && existing[j]; j++) {
            if (existing[j] != src_name[j]) { match = false; break; }
        }
        if (match && existing[j] == '\0' && j == name_len) {
            data_wr32(rep->data, 0, VIBE_ERR_NOSVC);
            rep->length = 4;
            return VIBE_ERR_NOSVC;
        }
    }

    static char name_pool[MAX_SERVICES * 32];
    static uint32_t pool_next = 0;

    if (pool_next + 32 > (uint32_t)sizeof(name_pool)) {
        data_wr32(rep->data, 0, VIBE_ERR_INTERNAL);
        rep->length = 4;
        return VIBE_ERR_INTERNAL;
    }

    char *dst = &name_pool[pool_next];
    for (uint32_t i = 0; i < name_len; i++) dst[i] = src_name[i];
    dst[name_len] = '\0';
    pool_next += 32;

    if (max_wasm == 0) max_wasm = 2 * 1024 * 1024;

    uint32_t new_id = service_count;
    services[new_id] = (service_entry_t){
        .name             = dst,
        .swappable        = true,
        .current_version  = 1,
        .max_wasm_bytes   = max_wasm,
    };
    service_count++;

    data_wr32(rep->data, 0, VIBE_OK);
    data_wr32(rep->data, 4, new_id);
    rep->length = 8;
    return SEL4_ERR_OK;
}

/* ── Handler: OP_VIBE_LIST_SERVICES ─────────────────────────────────────── */
static uint32_t handle_list_services(sel4_badge_t badge, const sel4_msg_t *req,
                                      sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;

    uint8_t *out = (uint8_t *)vibe_staging_vaddr;
    uint32_t out_max = STAGING_SIZE - 64;
    uint32_t pos = 0;

    for (uint32_t i = 0; i < service_count; i++) {
        const char *n = services[i].name;
        if (!n) continue;
        uint32_t j = 0;
        while (n[j] && pos + j + 1 < out_max) {
            out[pos + j] = (uint8_t)n[j];
            j++;
        }
        if (pos + j < out_max) out[pos + j] = 0;
        pos += j + 1;
    }
    agentos_wmb();

    data_wr32(rep->data, 0, service_count);
    data_wr32(rep->data, 4, 0u);
    data_wr32(rep->data, 8, pos);
    rep->length = 12;
    return SEL4_ERR_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * VibeOS Lifecycle Handlers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── MSG_VIBEOS_CREATE ─────────────────────────────────────────────────── */
static uint32_t handle_vos_create(sel4_badge_t badge, const sel4_msg_t *req,
                                   sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    uint32_t os_type   = data_rd32(req->data, 0);
    uint32_t ram_mb    = data_rd32(req->data, 4);
    uint32_t dev_flags = data_rd32(req->data, 8);

    if (os_type > 1) {
        data_wr32(rep->data, 0, VIBEOS_ERR_BAD_TYPE);
        rep->length = 4;
        return VIBEOS_ERR_BAD_TYPE;
    }
    if (ram_mb == 0 || ram_mb > 8192) {
        data_wr32(rep->data, 0, VIBEOS_ERR_OOM);
        rep->length = 4;
        return VIBEOS_ERR_OOM;
    }

    int slot = vos_alloc();
    if (slot < 0) {
        data_wr32(rep->data, 0, VIBEOS_ERR_OOM);
        rep->length = 4;
        return VIBEOS_ERR_OOM;
    }

    /* PPC to vm_manager: create VM slot */
    if (g_vmm_ep) {
        sel4_msg_t vreq = {0}, vrep = {0};
        vreq.opcode = OP_VM_CREATE;
        data_wr32(vreq.data, 0, 0u);      /* label_vaddr */
        data_wr32(vreq.data, 4, ram_mb);
        vreq.length = 8;
        sel4_call(g_vmm_ep, &vreq, &vrep);
        uint32_t vm_ok   = data_rd32(vrep.data, 0);
        uint32_t vm_slot = data_rd32(vrep.data, 4);
        if (vm_ok != 0) {
            dbg_puts("[vibe_engine] VOS_CREATE: vm_manager rejected CREATE\n");
            data_wr32(rep->data, 0, VIBEOS_ERR_OOM);
            rep->length = 4;
            return VIBEOS_ERR_OOM;
        }
        s_vos[slot].vm_slot = vm_slot;

        /* PPC to vm_manager: start the VM */
        sel4_msg_t sreq = {0}, srep = {0};
        sreq.opcode = OP_VM_START;
        data_wr32(sreq.data, 0, vm_slot);
        sreq.length = 4;
        sel4_call(g_vmm_ep, &sreq, &srep);
    }

    uint32_t handle = s_next_handle++;
    s_vos[slot].active   = true;
    s_vos[slot].handle   = handle;
    s_vos[slot].os_type  = (uint8_t)os_type;
    s_vos[slot].state    = (uint8_t)VIBEOS_STATE_BOOTING;
    s_vos[slot].ram_mb   = ram_mb;
    s_vos[slot].dev_mask = 0;
    s_vos[slot].swap_id  = 0;
    for (int i = 0; i < 5; i++)  s_vos[slot].dev_handles[i] = 0;
    for (int i = 0; i < 32; i++) s_vos[slot].module_hash[i] = 0;

    /* Open device handles via nameserver-resolved caps */
    if ((dev_flags & VIBEOS_DEV_SERIAL) && g_serial_ep) {
        sel4_msg_t dreq = {0}, drep = {0};
        dreq.opcode = MSG_SERIAL_OPEN;
        data_wr32(dreq.data, 0, 0u);  /* port_id */
        dreq.length = 4;
        sel4_call(g_serial_ep, &dreq, &drep);
        if (data_rd32(drep.data, 0) == 0) {
            s_vos[slot].dev_handles[0] = data_rd32(drep.data, 4);
            s_vos[slot].dev_mask |= VIBEOS_DEV_SERIAL;
        }
    }
    if ((dev_flags & VIBEOS_DEV_NET) && g_net_ep) {
        sel4_msg_t dreq = {0}, drep = {0};
        dreq.opcode = MSG_NET_OPEN;
        data_wr32(dreq.data, 0, 0u);  /* iface_id */
        dreq.length = 4;
        sel4_call(g_net_ep, &dreq, &drep);
        if (data_rd32(drep.data, 0) == 0) {
            s_vos[slot].dev_handles[1] = data_rd32(drep.data, 4);
            s_vos[slot].dev_mask |= VIBEOS_DEV_NET;
        }
    }
    if ((dev_flags & VIBEOS_DEV_BLOCK) && g_block_ep) {
        sel4_msg_t dreq = {0}, drep = {0};
        dreq.opcode = MSG_BLOCK_OPEN;
        data_wr32(dreq.data, 0, 0u);  /* dev_id */
        data_wr32(dreq.data, 4, 0u);  /* partition */
        dreq.length = 8;
        sel4_call(g_block_ep, &dreq, &drep);
        if (data_rd32(drep.data, 0) == 0) {
            s_vos[slot].dev_handles[2] = data_rd32(drep.data, 4);
            s_vos[slot].dev_mask |= VIBEOS_DEV_BLOCK;
        }
    }

    dbg_puts("[vibe_engine] VOS_CREATE: ok\n");
    data_wr32(rep->data, 0, VIBEOS_OK);
    data_wr32(rep->data, 4, handle);
    rep->length = 8;
    return SEL4_ERR_OK;
}

/* ── MSG_VIBEOS_DESTROY ────────────────────────────────────────────────── */
static uint32_t handle_vos_destroy(sel4_badge_t badge, const sel4_msg_t *req,
                                    sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    uint32_t handle = data_rd32(req->data, 0);
    int slot = vos_find(handle);
    if (slot < 0) {
        data_wr32(rep->data, 0, VIBEOS_ERR_NO_HANDLE);
        rep->length = 4;
        return VIBEOS_ERR_NO_HANDLE;
    }

    if (g_vmm_ep) {
        uint32_t vm_slot = s_vos[slot].vm_slot;
        sel4_msg_t sreq = {0}, srep = {0};
        sreq.opcode = OP_VM_STOP;
        data_wr32(sreq.data, 0, vm_slot);
        sreq.length = 4;
        sel4_call(g_vmm_ep, &sreq, &srep);

        sel4_msg_t dreq = {0}, drep = {0};
        dreq.opcode = OP_VM_DESTROY;
        data_wr32(dreq.data, 0, vm_slot);
        dreq.length = 4;
        sel4_call(g_vmm_ep, &dreq, &drep);
    }

    s_vos[slot].active = false;
    s_vos[slot].state  = (uint8_t)VIBEOS_STATE_DEAD;

    dbg_puts("[vibe_engine] VOS_DESTROY: ok\n");
    data_wr32(rep->data, 0, VIBEOS_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/* ── MSG_VIBEOS_STATUS ─────────────────────────────────────────────────── */
static uint32_t handle_vos_status(sel4_badge_t badge, const sel4_msg_t *req,
                                   sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    uint32_t handle = data_rd32(req->data, 0);
    int slot = vos_find(handle);
    if (slot < 0) {
        data_wr32(rep->data, 0, VIBEOS_ERR_NO_HANDLE);
        rep->length = 4;
        return VIBEOS_ERR_NO_HANDLE;
    }

    if (g_vmm_ep) {
        sel4_msg_t vreq = {0}, vrep = {0};
        vreq.opcode = OP_VM_INFO;
        data_wr32(vreq.data, 0, s_vos[slot].vm_slot);
        vreq.length = 4;
        sel4_call(g_vmm_ep, &vreq, &vrep);
        uint32_t vm_ok    = data_rd32(vrep.data, 0);
        uint32_t vm_state = data_rd32(vrep.data, 4);
        if (vm_ok == 0 && vm_state == 3 &&
            s_vos[slot].state == (uint8_t)VIBEOS_STATE_BOOTING) {
            s_vos[slot].state = (uint8_t)VIBEOS_STATE_RUNNING;
        }
    }

    data_wr32(rep->data,  0, VIBEOS_OK);
    data_wr32(rep->data,  4, handle);
    data_wr32(rep->data,  8, s_vos[slot].state);
    data_wr32(rep->data, 12, s_vos[slot].os_type);
    data_wr32(rep->data, 16, s_vos[slot].ram_mb);
    data_wr32(rep->data, 20, s_vos[slot].dev_mask);
    rep->length = 24;
    return SEL4_ERR_OK;
}

/* ── MSG_VIBEOS_LIST ───────────────────────────────────────────────────── */
static uint32_t handle_vos_list(sel4_badge_t badge, const sel4_msg_t *req,
                                 sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    uint32_t offset = data_rd32(req->data, 0);
    uint32_t count  = 0;
    uint32_t seen   = 0;

    for (int i = 0; i < MAX_VOS_INSTANCES && count < 16; i++) {
        if (!s_vos[i].active) continue;
        if (seen < offset) { seen++; continue; }
        data_wr32(rep->data, (int)(4 + count * 4), s_vos[i].handle);
        count++;
        seen++;
    }

    data_wr32(rep->data, 0, count);
    rep->length = (uint32_t)(4 + count * 4);
    return SEL4_ERR_OK;
}

/* ── MSG_VIBEOS_BIND_DEVICE / UNBIND_DEVICE ────────────────────────────── */
static uint32_t handle_vos_device(sel4_badge_t badge, const sel4_msg_t *req,
                                   sel4_msg_t *rep, void *ctx)
{
    uint32_t op      = (uint32_t)(uintptr_t)ctx;
    (void)badge;
    uint32_t handle   = data_rd32(req->data, 0);
    uint32_t dev_type = data_rd32(req->data, 4);

    int slot = vos_find(handle);
    if (slot < 0) {
        data_wr32(rep->data, 0, VIBEOS_ERR_NO_HANDLE);
        rep->length = 4;
        return VIBEOS_ERR_NO_HANDLE;
    }
    if (dev_type == 0 || (dev_type & (dev_type - 1)) != 0) {
        data_wr32(rep->data, 0, VIBEOS_ERR_BAD_TYPE);
        rep->length = 4;
        return VIBEOS_ERR_BAD_TYPE;
    }
    if (s_vos[slot].state == (uint8_t)VIBEOS_STATE_DEAD ||
        s_vos[slot].state == (uint8_t)VIBEOS_STATE_CREATING) {
        data_wr32(rep->data, 0, VIBEOS_ERR_WRONG_STATE);
        rep->length = 4;
        return VIBEOS_ERR_WRONG_STATE;
    }

    if (op == MSG_VIBEOS_BIND_DEVICE)
        s_vos[slot].dev_mask |= dev_type;
    else
        s_vos[slot].dev_mask &= ~dev_type;

    data_wr32(rep->data, 0, VIBEOS_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/* ── MSG_VIBEOS_SNAPSHOT ───────────────────────────────────────────────── */
static uint32_t handle_vos_snapshot(sel4_badge_t badge, const sel4_msg_t *req,
                                     sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    uint32_t handle = data_rd32(req->data, 0);
    int slot = vos_find(handle);
    if (slot < 0) {
        data_wr32(rep->data, 0, VIBEOS_ERR_NO_HANDLE);
        rep->length = 4;
        return VIBEOS_ERR_NO_HANDLE;
    }
    if (s_vos[slot].state != (uint8_t)VIBEOS_STATE_RUNNING &&
        s_vos[slot].state != (uint8_t)VIBEOS_STATE_PAUSED) {
        data_wr32(rep->data, 0, VIBEOS_ERR_WRONG_STATE);
        rep->length = 4;
        return VIBEOS_ERR_WRONG_STATE;
    }

    s_vos[slot].state = (uint8_t)VIBEOS_STATE_SNAPSHOT;
    uint32_t h_lo = 0, h_hi = 0;

    if (g_vmm_ep) {
        sel4_msg_t vreq = {0}, vrep = {0};
        vreq.opcode = OP_VM_SNAPSHOT;
        data_wr32(vreq.data, 0, s_vos[slot].vm_slot);
        vreq.length = 4;
        sel4_call(g_vmm_ep, &vreq, &vrep);
        uint32_t ok = data_rd32(vrep.data, 0);
        if (ok != 0) {
            s_vos[slot].state = (uint8_t)VIBEOS_STATE_RUNNING;
            data_wr32(rep->data, 0, VIBEOS_ERR_NOT_IMPL);
            rep->length = 4;
            return VIBEOS_ERR_NOT_IMPL;
        }
        h_lo = data_rd32(vrep.data, 4);
        h_hi = data_rd32(vrep.data, 8);
    }

    s_vos[slot].state = (uint8_t)VIBEOS_STATE_RUNNING;
    data_wr32(rep->data,  0, VIBEOS_OK);
    data_wr32(rep->data,  4, handle);
    data_wr32(rep->data,  8, h_lo);
    data_wr32(rep->data, 12, h_hi);
    rep->length = 16;
    return SEL4_ERR_OK;
}

/* ── MSG_VIBEOS_RESTORE ────────────────────────────────────────────────── */
static uint32_t handle_vos_restore(sel4_badge_t badge, const sel4_msg_t *req,
                                    sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    uint32_t handle  = data_rd32(req->data, 0);
    uint32_t snap_lo = data_rd32(req->data, 4);
    uint32_t snap_hi = data_rd32(req->data, 8);

    int slot = vos_find(handle);
    if (slot < 0) {
        data_wr32(rep->data, 0, VIBEOS_ERR_NO_HANDLE);
        rep->length = 4;
        return VIBEOS_ERR_NO_HANDLE;
    }

    if (g_vmm_ep) {
        sel4_msg_t vreq = {0}, vrep = {0};
        vreq.opcode = OP_VM_RESTORE;
        data_wr32(vreq.data,  0, s_vos[slot].vm_slot);
        data_wr32(vreq.data,  4, snap_lo);
        data_wr32(vreq.data,  8, snap_hi);
        vreq.length = 12;
        sel4_call(g_vmm_ep, &vreq, &vrep);
        uint32_t ok = data_rd32(vrep.data, 0);
        if (ok != 0) {
            data_wr32(rep->data, 0, VIBEOS_ERR_NOT_IMPL);
            rep->length = 4;
            return VIBEOS_ERR_NOT_IMPL;
        }
    }

    s_vos[slot].state = (uint8_t)VIBEOS_STATE_BOOTING;
    data_wr32(rep->data, 0, VIBEOS_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/* ── MSG_VIBEOS_MIGRATE — placeholder ──────────────────────────────────── */
static uint32_t handle_vos_migrate(sel4_badge_t badge, const sel4_msg_t *req,
                                    sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;
    data_wr32(rep->data, 0, VIBEOS_ERR_NOT_IMPL);
    rep->length = 4;
    return VIBEOS_ERR_NOT_IMPL;
}

/* ── MSG_VIBEOS_BOOT ───────────────────────────────────────────────────── */
static uint32_t handle_vos_boot(sel4_badge_t badge, const sel4_msg_t *req,
                                 sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    uint32_t handle = data_rd32(req->data, 0);
    int slot = vos_find(handle);
    if (slot < 0) {
        data_wr32(rep->data, 0, VIBEOS_ERR_NO_HANDLE);
        rep->length = 4;
        return VIBEOS_ERR_NO_HANDLE;
    }
    if (s_vos[slot].state != (uint8_t)VIBEOS_STATE_CREATING) {
        data_wr32(rep->data, 0, VIBEOS_ERR_WRONG_STATE);
        rep->length = 4;
        return VIBEOS_ERR_WRONG_STATE;
    }

    if (g_vmm_ep) {
        sel4_msg_t vreq = {0}, vrep = {0};
        vreq.opcode = OP_VM_START;
        data_wr32(vreq.data, 0, s_vos[slot].vm_slot);
        vreq.length = 4;
        sel4_call(g_vmm_ep, &vreq, &vrep);
    }

    s_vos[slot].state = (uint8_t)VIBEOS_STATE_BOOTING;
    data_wr32(rep->data, 0, VIBEOS_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/* ── MSG_VIBEOS_CONFIGURE ──────────────────────────────────────────────── */
static uint32_t handle_vos_configure(sel4_badge_t badge, const sel4_msg_t *req,
                                      sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    uint32_t handle        = data_rd32(req->data,  0);
    uint32_t new_ram       = data_rd32(req->data,  4);
    uint32_t new_budget_us = data_rd32(req->data,  8);
    uint32_t new_period_us = data_rd32(req->data, 12);

    int slot = vos_find(handle);
    if (slot < 0) {
        data_wr32(rep->data, 0, VIBEOS_ERR_NO_HANDLE);
        rep->length = 4;
        return VIBEOS_ERR_NO_HANDLE;
    }
    if (s_vos[slot].state == (uint8_t)VIBEOS_STATE_DEAD) {
        data_wr32(rep->data, 0, VIBEOS_ERR_WRONG_STATE);
        rep->length = 4;
        return VIBEOS_ERR_WRONG_STATE;
    }

    if (new_ram) s_vos[slot].ram_mb = new_ram;

    if (g_vmm_ep) {
        sel4_msg_t vreq = {0}, vrep = {0};
        vreq.opcode = OP_VM_CONFIGURE;
        data_wr32(vreq.data,  0, s_vos[slot].vm_slot);
        data_wr32(vreq.data,  4, new_ram);
        data_wr32(vreq.data,  8, new_budget_us);
        data_wr32(vreq.data, 12, new_period_us);
        vreq.length = 16;
        sel4_call(g_vmm_ep, &vreq, &vrep);
    }

    dbg_puts("[vibe_engine] VOS_CONFIGURE: ok\n");
    data_wr32(rep->data, 0, VIBEOS_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/* ── MSG_VIBEOS_LOAD_MODULE ────────────────────────────────────────────── */
static uint32_t handle_vibeos_load_module(sel4_badge_t badge, const sel4_msg_t *req,
                                           sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    uint32_t handle      = data_rd32(req->data, 0);
    uint32_t module_type = data_rd32(req->data, 4);
    uint32_t module_size = data_rd32(req->data, 8);

    int vslot = vos_find(handle);
    if (vslot < 0) {
        data_wr32(rep->data, 0, VIBEOS_ERR_BAD_HANDLE);
        data_wr32(rep->data, 4, 0u);
        rep->length = 8;
        return VIBEOS_ERR_BAD_HANDLE;
    }

    if (module_type != VIBEOS_MODULE_TYPE_WASM &&
        module_type != VIBEOS_MODULE_TYPE_ELF) {
        data_wr32(rep->data, 0, VIBEOS_ERR_BAD_MODULE_TYPE);
        data_wr32(rep->data, 4, 0u);
        rep->length = 8;
        return VIBEOS_ERR_BAD_MODULE_TYPE;
    }

    if (module_type == VIBEOS_MODULE_TYPE_WASM) {
        const uint8_t *staged = (const uint8_t *)vibe_staging_vaddr;
        if (!validate_wasm_header(staged, module_size)) {
            data_wr32(rep->data, 0, VIBEOS_ERR_WASM_LOAD_FAIL);
            data_wr32(rep->data, 4, 0u);
            rep->length = 8;
            return VIBEOS_ERR_WASM_LOAD_FAIL;
        }
    }

    uint32_t swap_id = next_proposal_id++;
    s_vos[vslot].swap_id = swap_id;

    volatile uint8_t *meta = (volatile uint8_t *)(vibe_staging_vaddr + STAGING_SIZE - 64);
    uint32_t svc = (uint32_t)handle;
    uint32_t off = 0;
    uint32_t sz  = module_size;
    uint32_t pid = swap_id;

    meta[0]  = svc & 0xff; meta[1]  = (svc >> 8) & 0xff;
    meta[2]  = (svc >> 16) & 0xff; meta[3]  = (svc >> 24) & 0xff;
    meta[4]  = off & 0xff; meta[5]  = (off >> 8) & 0xff;
    meta[6]  = (off >> 16) & 0xff; meta[7]  = (off >> 24) & 0xff;
    meta[8]  = sz & 0xff;  meta[9]  = (sz >> 8) & 0xff;
    meta[10] = (sz >> 16) & 0xff;  meta[11] = (sz >> 24) & 0xff;
    meta[12] = pid & 0xff; meta[13] = (pid >> 8) & 0xff;
    meta[14] = (pid >> 16) & 0xff; meta[15] = (pid >> 24) & 0xff;

    agentos_wmb();
    if (g_ctrl_ep) seL4_Signal(g_ctrl_ep);

    data_wr32(rep->data, 0, VIBEOS_OK);
    data_wr32(rep->data, 4, swap_id);
    rep->length = 8;
    return SEL4_ERR_OK;
}

/* ── MSG_VIBEOS_CHECK_SERVICE_EXISTS ───────────────────────────────────── */
static uint32_t handle_vibeos_check_service_exists(sel4_badge_t badge,
                                                    const sel4_msg_t *req,
                                                    sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;
    uint32_t func_class = data_rd32(req->data, 0);
    (void)func_class;
    /* Simplified: always report not found — full cap_policy integration
     * is handled by the capability broker service. */
    data_wr32(rep->data,  0, VIBEOS_OK);
    data_wr32(rep->data,  4, 0u);  /* exists = 0 */
    data_wr32(rep->data,  8, 0u);
    data_wr32(rep->data, 12, 0u);
    rep->length = 16;
    return SEL4_ERR_OK;
}

/* ── Test-only dispatch shim ────────────────────────────────────────────── */
#ifdef AGENTOS_TEST_HOST
/*
 * vibe_engine_test_init() — reset all state for a clean test run.
 */
static void vibe_engine_test_init(void)
{
    for (int i = 0; i < MAX_PROPOSALS; i++)
        proposals[i].state = PROP_STATE_FREE;
    for (int i = 0; i < MAX_VOS_INSTANCES; i++)
        s_vos[i].active = false;
    for (int i = 0; i < MAX_SERVICES; i++) {
        services[i].name            = (void *)0;
        services[i].swappable       = false;
        services[i].current_version = 0;
        services[i].max_wasm_bytes  = 0;
    }
    service_count     = 0;
    next_proposal_id  = 1;
    total_proposals   = 0;
    total_swaps       = 0;
    total_rejections  = 0;
    s_next_handle     = 1;
    g_ctrl_ep         = 0;
    g_vmm_ep          = 0;
    g_serial_ep       = 0;
    g_block_ep        = 0;
    g_net_ep          = 0;
    vibe_staging_vaddr = 0;
    register_services();
}

/*
 * vibe_engine_dispatch_one() — single-shot dispatch for host-side tests.
 *
 * Matches the sel4_handler_fn signature expected by sel4_server_dispatch
 * but routes via a local switch so tests can call individual handlers
 * without a live server loop.
 */
static uint32_t vibe_engine_dispatch_one(sel4_badge_t badge,
                                          const sel4_msg_t *req,
                                          sel4_msg_t *rep)
{
    switch (req->opcode) {
    case OP_VIBE_PROPOSE:           return handle_propose(badge, req, rep, (void*)0);
    case OP_VIBE_VALIDATE:          return handle_validate(badge, req, rep, (void*)0);
    case OP_VIBE_EXECUTE:           return handle_execute(badge, req, rep, (void*)0);
    case OP_VIBE_STATUS:            return handle_status(badge, req, rep, (void*)0);
    case OP_VIBE_ROLLBACK:          return handle_rollback(badge, req, rep, (void*)0);
    case OP_VIBE_HEALTH:            return handle_health(badge, req, rep, (void*)0);
    case OP_VIBE_REGISTER_SERVICE:  return handle_register_service(badge, req, rep, (void*)0);
    case OP_VIBE_LIST_SERVICES:     return handle_list_services(badge, req, rep, (void*)0);
    case MSG_VIBEOS_CREATE:         return handle_vos_create(badge, req, rep, (void*)0);
    case MSG_VIBEOS_DESTROY:        return handle_vos_destroy(badge, req, rep, (void*)0);
    case MSG_VIBEOS_STATUS:         return handle_vos_status(badge, req, rep, (void*)0);
    case MSG_VIBEOS_LIST:           return handle_vos_list(badge, req, rep, (void*)0);
    case MSG_VIBEOS_BIND_DEVICE:
        return handle_vos_device(badge, req, rep, (void*)(uintptr_t)MSG_VIBEOS_BIND_DEVICE);
    case MSG_VIBEOS_UNBIND_DEVICE:
        return handle_vos_device(badge, req, rep, (void*)(uintptr_t)MSG_VIBEOS_UNBIND_DEVICE);
    case MSG_VIBEOS_BOOT:           return handle_vos_boot(badge, req, rep, (void*)0);
    case MSG_VIBEOS_LOAD_MODULE:    return handle_vibeos_load_module(badge, req, rep, (void*)0);
    case MSG_VIBEOS_CHECK_SERVICE_EXISTS:
        return handle_vibeos_check_service_exists(badge, req, rep, (void*)0);
    case MSG_VIBEOS_SNAPSHOT:       return handle_vos_snapshot(badge, req, rep, (void*)0);
    case MSG_VIBEOS_RESTORE:        return handle_vos_restore(badge, req, rep, (void*)0);
    case MSG_VIBEOS_MIGRATE:        return handle_vos_migrate(badge, req, rep, (void*)0);
    case MSG_VIBEOS_CONFIGURE:      return handle_vos_configure(badge, req, rep, (void*)0);
    default:
        rep->opcode = SEL4_ERR_INVALID_OP;
        rep->length = 0;
        return SEL4_ERR_INVALID_OP;
    }
}
#endif /* AGENTOS_TEST_HOST */

/* ── Entry point ────────────────────────────────────────────────────────── */
#ifndef AGENTOS_TEST_HOST
/*
 * vibe_engine_main — called by root-task boot dispatcher.
 *
 * Parameters:
 *   my_ep   — this PD's receive endpoint
 *   ns_ep   — nameserver endpoint for service lookup
 *   ctrl_ep — direct controller notification cap (fixes invalid-channel-30 bug)
 *
 * Replaces: void init(void) / microkit_msginfo protected(ch, msg) / void notified(ch)
 */
void vibe_engine_main(seL4_CPtr my_ep, seL4_CPtr ns_ep, seL4_CPtr ctrl_ep)
{
    dbg_puts("[vibe_engine] VibeEngine PD starting (raw seL4 IPC)\n");

    /* Initialise tables */
    for (int i = 0; i < MAX_PROPOSALS; i++)
        proposals[i].state = PROP_STATE_FREE;
    for (int i = 0; i < MAX_VOS_INSTANCES; i++)
        s_vos[i].active = false;
    register_services();

    /* Store direct controller cap — no more stale channel 30 */
    g_ctrl_ep = ctrl_ep;

    /* Resolve device endpoints via nameserver.
     * These replace stale CH_SERIAL_PD=44, CH_BLOCK_PD=49, CH_NET_PD=48. */
    g_serial_ep = lookup_service(ns_ep, "serial");
    g_block_ep  = lookup_service(ns_ep, "block_pd");
    g_net_ep    = lookup_service(ns_ep, "net_pd");
    g_vmm_ep    = lookup_service(ns_ep, "vm_manager");

    /* Register ourselves */
    register_with_nameserver(ns_ep);

    /* Build the server dispatch table */
    sel4_server_init(&g_srv, my_ep);

    sel4_server_register(&g_srv, OP_VIBE_PROPOSE,           handle_propose,           (void*)0);
    sel4_server_register(&g_srv, OP_VIBE_VALIDATE,          handle_validate,          (void*)0);
    sel4_server_register(&g_srv, OP_VIBE_EXECUTE,           handle_execute,           (void*)0);
    sel4_server_register(&g_srv, OP_VIBE_STATUS,            handle_status,            (void*)0);
    sel4_server_register(&g_srv, OP_VIBE_ROLLBACK,          handle_rollback,          (void*)0);
    sel4_server_register(&g_srv, OP_VIBE_HEALTH,            handle_health,            (void*)0);
    sel4_server_register(&g_srv, OP_VIBE_REGISTER_SERVICE,  handle_register_service,  (void*)0);
    sel4_server_register(&g_srv, OP_VIBE_LIST_SERVICES,     handle_list_services,     (void*)0);

    sel4_server_register(&g_srv, MSG_VIBEOS_CREATE,
                         handle_vos_create,   (void*)0);
    sel4_server_register(&g_srv, MSG_VIBEOS_DESTROY,
                         handle_vos_destroy,  (void*)0);
    sel4_server_register(&g_srv, MSG_VIBEOS_STATUS,
                         handle_vos_status,   (void*)0);
    sel4_server_register(&g_srv, MSG_VIBEOS_LIST,
                         handle_vos_list,     (void*)0);
    sel4_server_register(&g_srv, MSG_VIBEOS_BIND_DEVICE,
                         handle_vos_device,   (void*)(uintptr_t)MSG_VIBEOS_BIND_DEVICE);
    sel4_server_register(&g_srv, MSG_VIBEOS_UNBIND_DEVICE,
                         handle_vos_device,   (void*)(uintptr_t)MSG_VIBEOS_UNBIND_DEVICE);
    sel4_server_register(&g_srv, MSG_VIBEOS_BOOT,
                         handle_vos_boot,     (void*)0);
    sel4_server_register(&g_srv, MSG_VIBEOS_LOAD_MODULE,
                         handle_vibeos_load_module, (void*)0);
    sel4_server_register(&g_srv, MSG_VIBEOS_CHECK_SERVICE_EXISTS,
                         handle_vibeos_check_service_exists, (void*)0);
    sel4_server_register(&g_srv, MSG_VIBEOS_SNAPSHOT,
                         handle_vos_snapshot, (void*)0);
    sel4_server_register(&g_srv, MSG_VIBEOS_RESTORE,
                         handle_vos_restore,  (void*)0);
    sel4_server_register(&g_srv, MSG_VIBEOS_MIGRATE,
                         handle_vos_migrate,  (void*)0);
    sel4_server_register(&g_srv, MSG_VIBEOS_CONFIGURE,
                         handle_vos_configure,(void*)0);

    dbg_puts("[vibe_engine] *** VibeEngine ALIVE — accepting proposals ***\n");
    sel4_server_run(&g_srv);  /* NEVER RETURNS */
}

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep) { vibe_engine_main(my_ep, ns_ep, 0u); }

#endif /* !AGENTOS_TEST_HOST */
