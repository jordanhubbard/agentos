/*
 * app_slot.c — Generic Application Slot Protection Domain — E5-S5: raw seL4 IPC
 *
 * All four pre-allocated app slot PDs (app_slot_0..3) run this binary.
 * SpawnServer stages an ELF in spawn_elf_shmem, writes a spawn_header_t
 * at offset 0 of the region, then calls the slot's endpoint (spawn_ep).
 * The slot reads the header, validates the ELF magic and SHA-256 hash,
 * notifies SpawnServer, and enters IDLE again.
 *
 * The spawn_ep is the direct cap from spawn_server (root task distributes
 * at boot).  The slot also registers "app_slot_N" with the nameserver for
 * monitoring purposes, but uses the direct spawn_ep for spawn-server calls.
 *
 * Entry point:
 *   void app_slot_main(seL4_CPtr my_ep, seL4_CPtr ns_ep, seL4_CPtr spawn_ep)
 *
 * Priority: 70 (below all system services, above idle).
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* ── Conditional compilation ─────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST
/*
 * Host-side test build: minimal stubs — no seL4 or Microkit headers.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef unsigned long      seL4_CPtr;
typedef unsigned long long sel4_badge_t;

typedef struct {
    uint32_t opcode;
    uint32_t length;
    uint8_t  data[48];
} sel4_msg_t;

#define SEL4_ERR_OK          0u
#define SEL4_ERR_INVALID_OP  1u
#define SEL4_ERR_NOT_FOUND   2u
#define SEL4_ERR_BAD_ARG     4u
#define SEL4_ERR_NO_MEM      5u
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

/* seL4_DebugPutChar stub */
static inline void seL4_DebugPutChar(char c) { (void)c; }

/* sel4_call stub */
static sel4_msg_t _test_last_call_rep;
static inline void sel4_call(seL4_CPtr ep, const sel4_msg_t *req, sel4_msg_t *rep)
{
    (void)ep; (void)req;
    *rep = _test_last_call_rep;
}

#else /* !AGENTOS_TEST_HOST — production build */

#include <stdint.h>
#include <stdbool.h>
#include "sel4_ipc.h"     /* sel4_msg_t, sel4_badge_t, SEL4_ERR_* */
#include "sel4_server.h"  /* sel4_server_t, sel4_server_init/register/run */
#include <sel4/sel4.h>    /* seL4_DebugPutChar */

#endif /* AGENTOS_TEST_HOST */

/* ── Contract opcodes ────────────────────────────────────────────────────── */

#ifndef OP_SPAWN_LAUNCH
#define OP_SPAWN_LAUNCH     0xA0u
#endif
#ifndef OP_SPAWN_KILL
#define OP_SPAWN_KILL       0xA1u
#endif
#ifndef OP_APPSLOT_STATUS
#define OP_APPSLOT_STATUS   0xA5u  /* query app slot state */
#endif
#ifndef SPAWN_MAGIC
#define SPAWN_MAGIC         0x5350574Eu  /* "SPWN" */
#endif
#ifndef SPAWN_HEADER_SIZE
#define SPAWN_HEADER_SIZE   96u
#endif
#ifndef SPAWN_MAX_ELF_SIZE
#define SPAWN_MAX_ELF_SIZE  0x7FF40u
#endif
#ifndef OP_NS_REGISTER
#define OP_NS_REGISTER      0xD0u
#endif
#ifndef NS_NAME_MAX
#define NS_NAME_MAX         32
#endif
#ifndef TRACE_PD_APP_SLOT
#define TRACE_PD_APP_SLOT   33u
#endif

/* App slot states */
#define APPSLOT_STATE_IDLE    0u
#define APPSLOT_STATE_LOADING 1u
#define APPSLOT_STATE_RUNNING 2u
#define APPSLOT_STATE_FAILED  3u
#define APPSLOT_STATE_KILLED  4u

/* ── Shared memory ───────────────────────────────────────────────────────── */

/*
 * spawn_elf_shmem_vaddr — set by root task before calling app_slot_main().
 * In test builds, set directly by the test harness.
 */
uintptr_t spawn_elf_shmem_vaddr;

/* ── Module state ────────────────────────────────────────────────────────── */

static struct {
    uint32_t  slot_index;
    uint32_t  state;          /* APPSLOT_STATE_* */
    uint32_t  app_id;
    bool      slot_failed;
} g_slot = { 0, APPSLOT_STATE_IDLE, 0, false };

/* Direct endpoint cap to spawn_server (passed at boot) */
static seL4_CPtr g_spawn_ep = 0;

/* Server instance */
static sel4_server_t g_srv;

/* ── Data field helpers ───────────────────────────────────────────────────── */

static inline uint32_t data_rd32(const uint8_t *d, int off)
{
    return (uint32_t)d[off    ]
         | ((uint32_t)d[off+1] <<  8)
         | ((uint32_t)d[off+2] << 16)
         | ((uint32_t)d[off+3] << 24);
}

static inline void data_wr32(uint8_t *d, int off, uint32_t v)
{
    d[off  ] = (uint8_t)(v      );
    d[off+1] = (uint8_t)(v >>  8);
    d[off+2] = (uint8_t)(v >> 16);
    d[off+3] = (uint8_t)(v >> 24);
}

/* ── Debug output ────────────────────────────────────────────────────────── */

static void dbg_puts(const char *s)
{
    for (; *s; s++)
        seL4_DebugPutChar(*s);
}

/* ── Nameserver registration ─────────────────────────────────────────────── */

static void register_with_nameserver(seL4_CPtr ns_ep, uint32_t slot_index)
{
    if (!ns_ep) return;
    sel4_msg_t req, rep;
    req.opcode = OP_NS_REGISTER;
    data_wr32(req.data, 0,  0u);
    data_wr32(req.data, 4,  TRACE_PD_APP_SLOT);
    data_wr32(req.data, 8,  0u);
    data_wr32(req.data, 12, 1u);
    /* Build "app_slot_N" */
    const char prefix[] = "app_slot_";
    int pi = 0;
    for (; prefix[pi]; pi++)
        req.data[16 + pi] = (uint8_t)prefix[pi];
    req.data[16 + pi++] = (uint8_t)('0' + (slot_index & 0xFu));
    for (int ni = pi; ni < NS_NAME_MAX; ni++)
        req.data[16 + ni] = 0;
    req.length = 48u;
    sel4_call(ns_ep, &req, &rep);
}

/* ── SHA-256 placeholder ─────────────────────────────────────────────────── */

/*
 * In production this calls sha256_mini() from sha256_mini.c.
 * In test builds we provide a trivial stub that always sets hash to 0.
 * The real sha256_mini.h is not included here to keep the test compile simple.
 */
#ifdef AGENTOS_TEST_HOST
static void compute_sha256(const uint8_t *data, uint32_t len, uint8_t out[32])
{
    (void)data; (void)len;
    for (int i = 0; i < 32; i++) out[i] = 0;
}
#else
/* Forward declaration — sha256_mini.c is compiled separately */
extern void sha256_mini(const uint8_t *data, uint32_t len, uint8_t *out);
static void compute_sha256(const uint8_t *data, uint32_t len, uint8_t out[32])
{
    sha256_mini(data, len, out);
}
#endif

/* ── ELF staging validation ───────────────────────────────────────────────── */

/*
 * validate_and_load — validate the ELF image in spawn_elf_shmem.
 * Returns APPSLOT_STATE_RUNNING on success, APPSLOT_STATE_FAILED on error.
 */
static uint32_t validate_and_load(uint32_t app_id)
{
    if (!spawn_elf_shmem_vaddr) {
        dbg_puts("[app_slot] error: spawn_elf_shmem not mapped\n");
        return APPSLOT_STATE_FAILED;
    }

    const volatile uint32_t *hdr32 =
        (const volatile uint32_t *)spawn_elf_shmem_vaddr;

    uint32_t magic    = hdr32[0];
    uint32_t elf_size = hdr32[1];
    uint32_t hdr_app_id = hdr32[3];

    if (magic != SPAWN_MAGIC) {
        dbg_puts("[app_slot] error: bad magic in spawn header\n");
        return APPSLOT_STATE_FAILED;
    }

    if (elf_size == 0u || elf_size > SPAWN_MAX_ELF_SIZE) {
        dbg_puts("[app_slot] error: invalid elf_size in spawn header\n");
        return APPSLOT_STATE_FAILED;
    }

    if (hdr_app_id != app_id) {
        dbg_puts("[app_slot] error: app_id mismatch in spawn header\n");
        return APPSLOT_STATE_FAILED;
    }

    /* ELF magic check */
    const uint8_t *elf_bytes =
        (const uint8_t *)(spawn_elf_shmem_vaddr + SPAWN_HEADER_SIZE);
    if (elf_size >= 4u &&
        !(elf_bytes[0] == 0x7Fu && elf_bytes[1] == 'E' &&
          elf_bytes[2] == 'L'  && elf_bytes[3] == 'F')) {
        dbg_puts("[app_slot] error: ELF magic mismatch\n");
        return APPSLOT_STATE_FAILED;
    }

    /*
     * SHA-256 verification: compare computed hash against header's elf_sha256.
     * Header elf_sha256[32] is at byte offset 64 (after 6 × uint32 = 24 bytes,
     * plus name[32] = 56 bytes, plus elf_sha256 starts at 56).
     * spawn_header_t layout (packed): magic(4)+elf_size(4)+cap_classes(4)+app_id(4)+
     * vfs_handle(4)+net_vnic_id(4) = 24 bytes of uint32, then name[32] at 24,
     * then elf_sha256[32] at 56.
     */
    const uint8_t *expected_hash =
        (const uint8_t *)(spawn_elf_shmem_vaddr + 56u);
    uint8_t computed[32];
    compute_sha256(elf_bytes, elf_size, computed);

    bool hash_ok = true;
    for (int i = 0; i < 32; i++) {
        if (computed[i] != expected_hash[i]) {
            hash_ok = false;
            break;
        }
    }

    if (!hash_ok) {
        dbg_puts("[app_slot] SECURITY: ELF SHA-256 mismatch — aborting load\n");
        return APPSLOT_STATE_FAILED;
    }

    dbg_puts("[app_slot] ELF validated — slot running\n");

    /*
     * MVP: Acknowledge to spawn_server via the direct spawn_ep.
     * Production: parse ELF, set up TCB registers, seL4_TCB_WriteRegisters + Resume.
     */
    return APPSLOT_STATE_RUNNING;
}

/* ── IPC handlers ────────────────────────────────────────────────────────── */

/*
 * handle_spawn_launch — OP_SPAWN_LAUNCH (0xA0)
 *
 * Sent by spawn_server to wake up this slot and trigger ELF load.
 *
 * Request:
 *   data[0..3] = app_id       (uint32)
 *   data[4..7] = cap_classes  (uint32)
 *
 * Reply:
 *   data[0..3] = SEL4_ERR_OK or SEL4_ERR_INTERNAL
 *   data[4..7] = slot_index
 */
static uint32_t handle_spawn_launch(sel4_badge_t badge, const sel4_msg_t *req,
                                     sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t app_id = data_rd32(req->data, 0);

    g_slot.app_id     = app_id;
    g_slot.state      = APPSLOT_STATE_LOADING;
    g_slot.slot_failed = false;

    uint32_t new_state = validate_and_load(app_id);
    g_slot.state       = new_state;
    g_slot.slot_failed = (new_state == APPSLOT_STATE_FAILED);

    data_wr32(rep->data, 0, g_slot.slot_failed ? (uint32_t)SEL4_ERR_INTERNAL
                                                : (uint32_t)SEL4_ERR_OK);
    data_wr32(rep->data, 4, g_slot.slot_index);
    rep->length = 8u;
    return g_slot.slot_failed ? SEL4_ERR_INTERNAL : SEL4_ERR_OK;
}

/*
 * handle_spawn_kill — OP_SPAWN_KILL (0xA1)
 *
 * Sent by spawn_server to tear down this slot.
 *
 * Request:
 *   data[0..3] = app_id  (uint32)
 *
 * Reply:
 *   data[0..3] = SEL4_ERR_OK
 *   data[4..7] = slot_index
 */
static uint32_t handle_spawn_kill(sel4_badge_t badge, const sel4_msg_t *req,
                                   sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    (void)data_rd32(req->data, 0); /* app_id (informational) */

    g_slot.state       = APPSLOT_STATE_KILLED;
    g_slot.app_id      = 0;
    g_slot.slot_failed = false;

    dbg_puts("[app_slot] slot killed — resetting to IDLE\n");

    /* Reset to idle immediately — self-resetting slot */
    g_slot.state = APPSLOT_STATE_IDLE;

    data_wr32(rep->data, 0, (uint32_t)SEL4_ERR_OK);
    data_wr32(rep->data, 4, g_slot.slot_index);
    rep->length = 8u;
    return SEL4_ERR_OK;
}

/*
 * handle_appslot_status — OP_APPSLOT_STATUS (0xA5)
 *
 * Reply:
 *   data[0..3]  = slot_index  (uint32)
 *   data[4..7]  = state       (APPSLOT_STATE_*)
 *   data[8..11] = app_id      (uint32)
 *   data[12..15] = slot_failed (uint32, 0 or 1)
 */
static uint32_t handle_appslot_status(sel4_badge_t badge, const sel4_msg_t *req,
                                       sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;

    data_wr32(rep->data, 0,  g_slot.slot_index);
    data_wr32(rep->data, 4,  g_slot.state);
    data_wr32(rep->data, 8,  g_slot.app_id);
    data_wr32(rep->data, 12, g_slot.slot_failed ? 1u : 0u);
    rep->length = 16u;
    return SEL4_ERR_OK;
}

/* ── Test-visible helpers ────────────────────────────────────────────────── */

static void app_slot_test_init(uint32_t slot_index)
{
    g_slot.slot_index  = slot_index;
    g_slot.state       = APPSLOT_STATE_IDLE;
    g_slot.app_id      = 0;
    g_slot.slot_failed = false;
    g_spawn_ep         = 0;
    spawn_elf_shmem_vaddr = 0;

    sel4_server_init(&g_srv, 0);
    sel4_server_register(&g_srv, OP_SPAWN_LAUNCH,   handle_spawn_launch,  NULL);
    sel4_server_register(&g_srv, OP_SPAWN_KILL,     handle_spawn_kill,    NULL);
    sel4_server_register(&g_srv, OP_APPSLOT_STATUS, handle_appslot_status, NULL);
}

static uint32_t app_slot_dispatch_one(sel4_badge_t badge,
                                       const sel4_msg_t *req,
                                       sel4_msg_t *rep)
{
    return sel4_server_dispatch(&g_srv, badge, req, rep);
}

static uint32_t app_slot_get_state(void)  { return g_slot.state; }
static bool     app_slot_get_failed(void) { return g_slot.slot_failed; }
static uint32_t app_slot_get_index(void)  { return g_slot.slot_index; }

/* ── Entry point ─────────────────────────────────────────────────────────── */

#ifndef AGENTOS_TEST_HOST
void app_slot_main(seL4_CPtr my_ep, seL4_CPtr ns_ep, seL4_CPtr spawn_ep)
{
    dbg_puts("[app_slot] ready, waiting for ELF staging notification\n");

    g_spawn_ep = spawn_ep;

    /*
     * The slot_index is not passed as a named argument — the root task
     * differentiates instances by the my_ep cap value.  For registration
     * we use a sentinel 0; monitoring systems can correlate via app_id.
     */
    register_with_nameserver(ns_ep, 0u);

    g_slot.state       = APPSLOT_STATE_IDLE;
    g_slot.slot_failed = false;

    sel4_server_init(&g_srv, my_ep);
    sel4_server_register(&g_srv, OP_SPAWN_LAUNCH,   handle_spawn_launch,   NULL);
    sel4_server_register(&g_srv, OP_SPAWN_KILL,     handle_spawn_kill,     NULL);
    sel4_server_register(&g_srv, OP_APPSLOT_STATUS, handle_appslot_status, NULL);

    /* Enter server loop — never returns */
    sel4_server_run(&g_srv);
}
#endif /* !AGENTOS_TEST_HOST */
