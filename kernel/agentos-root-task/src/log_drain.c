/*
 * agentOS Log Drain Protection Domain — E5-S1: raw seL4 IPC
 *
 * Drains per-PD log ring buffers to the debug UART.  Each PD writes log
 * lines into its own 4KB ring in the shared log_drain_rings region and
 * calls log_drain via IPC to flush it.
 *
 * Ring layout (per PD, 4KB each):
 *   [0..3]     magic (LOG_RING_MAGIC)
 *   [4..7]     pd_id (which PD owns this ring)
 *   [8..11]    head  (write offset, updated by PD)
 *   [12..15]   tail  (read offset, updated by log_drain)
 *   [16..4095] circular character buffer (4080 bytes)
 *
 * IPC Protocol (raw seL4, sel4_server_t dispatch):
 *   Opcode in req->opcode:
 *     OP_LOG_WRITE  (0x87) - data[0..3]=slot, data[4..7]=pd_id
 *     OP_LOG_STATUS (0x86) - reply: data[0..3]=ring_count, data[4..11]=total_bytes
 *
 * Entry point:
 *   void log_drain_main(seL4_CPtr my_ep, seL4_CPtr ns_ep, seL4_CPtr serial_ep)
 *
 * Priority: 160 (above workers/agents, below eventbus — drain promptly)
 * Mode: passive (only runs when called via seL4 IPC)
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
#define SEL4_ERR_BAD_ARG     4u

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

/* Stub: sel4_call is a no-op in test builds; serial_ep calls do nothing */
static inline void sel4_call(seL4_CPtr ep, const sel4_msg_t *req, sel4_msg_t *rep)
{
    (void)ep; (void)req;
    /* In tests, simulate SERIAL_OK reply so serial_ready stays usable */
    rep->opcode = 0; /* SERIAL_OK */
    rep->data[0] = 0; rep->data[1] = 0; rep->data[2] = 0; rep->data[3] = 0; /* ok=0 */
    rep->data[4] = 1; rep->data[5] = 0; rep->data[6] = 0; rep->data[7] = 0; /* slot=1 */
    rep->length = 8;
}

/* seL4_DebugPutChar stub */
static inline void seL4_DebugPutChar(char c) { (void)c; }

#else /* !AGENTOS_TEST_HOST — production build */

#include <stdint.h>
#include <stdbool.h>
#include "sel4_server.h"    /* sel4_server_t, sel4_server_init/register/run */
#include "sel4_client.h"    /* sel4_client_t, sel4_client_call */
#include "sel4_ipc.h"       /* sel4_msg_t, sel4_badge_t, SEL4_ERR_* */
#include <sel4/sel4.h>      /* seL4_DebugPutChar */

#endif /* AGENTOS_TEST_HOST */

/* ── Contract opcodes ──────────────────────────────────────────────────────── */

#ifndef OP_LOG_WRITE
#define OP_LOG_WRITE   0x87u   /* register ring + drain */
#endif
#ifndef OP_LOG_STATUS
#define OP_LOG_STATUS  0x86u   /* return ring_count + total_bytes */
#endif

/* Nameserver opcode (used to self-register at startup) */
#ifndef OP_NS_REGISTER
#define OP_NS_REGISTER 0xD0u
#endif
#ifndef NS_OK
#define NS_OK 0u
#endif
#ifndef NS_NAME_MAX
#define NS_NAME_MAX 32
#endif

/* Serial service opcodes */
#ifndef MSG_SERIAL_OPEN
#define MSG_SERIAL_OPEN  0x2001u
#endif
#ifndef MSG_SERIAL_WRITE
#define MSG_SERIAL_WRITE 0x2003u
#endif
#ifndef SERIAL_OK
#define SERIAL_OK 0u
#endif
#ifndef SERIAL_MAX_WRITE_BYTES
#define SERIAL_MAX_WRITE_BYTES 256u
#endif

/* ── Ring buffer constants ─────────────────────────────────────────────────── */

#define LOG_RING_MAGIC    0xC0DE4D55u
#define MAX_LOG_RINGS     16
#define RING_SIZE         4096
#define RING_HEADER_SIZE  16
#define RING_BUF_SIZE     (RING_SIZE - RING_HEADER_SIZE)

/* ── Ring header layout (matches log_ring_header_t in agentos.h) ─────────── */

typedef struct {
    uint32_t magic;
    uint32_t pd_id;
    uint32_t head;   /* write offset (PD writes) */
    uint32_t tail;   /* read offset  (log_drain writes) */
} ld_ring_header_t;

/* ── Per-ring state ──────────────────────────────────────────────────────── */

typedef struct {
    bool     active;
    uint32_t pd_id;
    uint64_t bytes_total;
} log_ring_state_t;

/* ── Module state ─────────────────────────────────────────────────────────── */

/*
 * log_drain_rings_vaddr — base virtual address of the shared log ring region.
 * In production this is set by the root task before calling log_drain_main().
 * In test builds it is set directly by the test harness.
 */
uintptr_t log_drain_rings_vaddr;

/*
 * serial_shmem_vaddr — shared memory for serial_pd I/O transfer.
 * Set by the root task in production; in test builds set to 0 (unused).
 */
uintptr_t serial_shmem_vaddr;

static log_ring_state_t ring_states[MAX_LOG_RINGS];
static uint32_t         ring_count  = 0;
static uint64_t         total_bytes = 0;

/* Serial output state */
static uint32_t  serial_slot  = (uint32_t)-1;
static bool      serial_ready = false;
static seL4_CPtr g_serial_ep  = 0;

/* Server instance */
static sel4_server_t g_srv;

/* ── PD name table ─────────────────────────────────────────────────────────── */

static const struct { uint32_t id; const char *name; } pd_names[] = {
    { 0,  "controller" },
    { 1,  "event_bus"  },
    { 2,  "init_agent" },
    { 3,  "agentfs"    },
    { 4,  "vibe_engine"},
    { 5,  "worker_0"   },
    { 6,  "worker_1"   },
    { 7,  "worker_2"   },
    { 8,  "worker_3"   },
    { 9,  "swap_slot_0"},
    { 10, "swap_slot_1"},
    { 11, "swap_slot_2"},
    { 12, "swap_slot_3"},
    { 13, "log_drain"  },
    { 14, "linux_vmm"  },
    { 15, "fault_hndlr"},
};
#define NUM_PD_NAMES (sizeof(pd_names) / sizeof(pd_names[0]))

/* ── Data-field helpers (little-endian uint32 / uint64 in sel4_msg_t.data) ── */

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

static inline void data_wr64(uint8_t *d, int off, uint64_t v)
{
    data_wr32(d, off,     (uint32_t)(v & 0xFFFFFFFFu));
    data_wr32(d, off + 4, (uint32_t)(v >> 32));
}

/* ── Ring accessors ─────────────────────────────────────────────────────────── */

static volatile ld_ring_header_t *get_ring(uint32_t slot)
{
    return (volatile ld_ring_header_t *)
        (log_drain_rings_vaddr + (slot * RING_SIZE));
}

static volatile char *get_ring_buf(uint32_t slot)
{
    return (volatile char *)
        (log_drain_rings_vaddr + (slot * RING_SIZE) + RING_HEADER_SIZE);
}

/* ── PD name lookup ─────────────────────────────────────────────────────────── */

static const char *pd_name_for(uint32_t pd_id)
{
    for (uint32_t i = 0; i < NUM_PD_NAMES; i++) {
        if (pd_names[i].id == pd_id) return pd_names[i].name;
    }
    return "unknown";
}

/* ── Ring state helpers ─────────────────────────────────────────────────────── */

static log_ring_state_t *find_ring_state(uint32_t pd_id)
{
    for (uint32_t i = 0; i < ring_count; i++) {
        if (ring_states[i].active && ring_states[i].pd_id == pd_id)
            return &ring_states[i];
    }
    return NULL;
}

static log_ring_state_t *get_or_create_ring_state(uint32_t pd_id)
{
    log_ring_state_t *s = find_ring_state(pd_id);
    if (s) return s;
    if (ring_count >= MAX_LOG_RINGS) return NULL;
    s = &ring_states[ring_count++];
    s->active      = true;
    s->pd_id       = pd_id;
    s->bytes_total = 0;
    return s;
}

/* ── Debug output (no-libc, bare-metal) ─────────────────────────────────────── */

static void dbg_puts(const char *s)
{
    for (; *s; s++)
        seL4_DebugPutChar(*s);
}

/* ── Serial output ─────────────────────────────────────────────────────────── */

/*
 * try_serial_init — attempt to open a serial_pd slot on first use.
 *
 * Calls MSG_SERIAL_OPEN on g_serial_ep via sel4_call().
 * Falls back to seL4_DebugPutChar if serial_pd is not reachable.
 */
static void try_serial_init(void)
{
    if (serial_ready || !g_serial_ep || !serial_shmem_vaddr) return;

    sel4_msg_t req, rep;
    req.opcode = MSG_SERIAL_OPEN;
    req.length = 8;
    data_wr32(req.data, 0, MSG_SERIAL_OPEN);
    data_wr32(req.data, 4, 0);   /* port_id 0 */
    for (uint32_t i = 8; i < 48; i++) req.data[i] = 0;

    sel4_call(g_serial_ep, &req, &rep);

    uint32_t ok   = data_rd32(rep.data, 0);
    uint32_t slot = data_rd32(rep.data, 4);

    if (ok == SERIAL_OK) {
        serial_slot  = slot;
        serial_ready = true;
    }
}

/*
 * uart_puts — write NUL-terminated string to serial output.
 *
 * Uses serial_pd IPC when available; falls back to seL4_DebugPutChar loop.
 */
static void uart_puts(const char *s)
{
    if (!serial_ready) try_serial_init();

    if (!serial_ready) {
        dbg_puts(s);
        return;
    }

    uint8_t *shmem = (uint8_t *)serial_shmem_vaddr;
    while (*s) {
        uint32_t n = 0;
        while (n < SERIAL_MAX_WRITE_BYTES && s[n]) n++;

        for (uint32_t i = 0; i < n; i++)
            shmem[i] = (uint8_t)s[i];

        sel4_msg_t req, rep;
        req.opcode = MSG_SERIAL_WRITE;
        req.length = 12;
        data_wr32(req.data, 0, MSG_SERIAL_WRITE);
        data_wr32(req.data, 4, serial_slot);
        data_wr32(req.data, 8, n);
        for (uint32_t i = 12; i < 48; i++) req.data[i] = 0;
        sel4_call(g_serial_ep, &req, &rep);

        s += n;
    }
}

static void uart_tagged_line(const char *pd_name, const char *line)
{
    uart_puts("\033[36m[");
    uart_puts(pd_name);
    uart_puts("]\033[0m ");
    uart_puts(line);
    uart_puts("\n");
}

/* ── Ring drain ─────────────────────────────────────────────────────────────── */

static uint32_t drain_ring(uint32_t slot, log_ring_state_t *rs)
{
    volatile ld_ring_header_t *hdr = get_ring(slot);
    volatile char *buf = get_ring_buf(slot);

    if (hdr->magic != LOG_RING_MAGIC) return 0;

    uint32_t head    = hdr->head;
    uint32_t tail    = hdr->tail;
    uint32_t drained = 0;

    static char line_buf[256];
    static uint32_t line_pos = 0;

    const char *name = pd_name_for(rs->pd_id);

    while (tail != head) {
        char c = buf[tail % RING_BUF_SIZE];
        tail = (tail + 1) % RING_BUF_SIZE;
        drained++;

        if (c == '\n' || line_pos >= (uint32_t)(sizeof(line_buf) - 1)) {
            line_buf[line_pos] = '\0';
            uart_tagged_line(name, line_buf);
            line_pos = 0;
        } else {
            line_buf[line_pos++] = c;
        }
    }

    hdr->tail        = tail;
    rs->bytes_total += drained;
    total_bytes     += drained;
    return drained;
}

static void drain_all(void)
{
    for (uint32_t i = 0; i < ring_count; i++) {
        if (ring_states[i].active)
            drain_ring(i, &ring_states[i]);
    }
}

/* ── Ring initialisation ────────────────────────────────────────────────────── */

static void init_rings(void)
{
    for (uint32_t i = 0; i < MAX_LOG_RINGS; i++) {
        volatile ld_ring_header_t *hdr = get_ring(i);
        hdr->magic = 0;
        hdr->pd_id = 0;
        hdr->head  = 0;
        hdr->tail  = 0;
    }
}

static void register_ring(uint32_t slot, uint32_t pd_id)
{
    volatile ld_ring_header_t *hdr = get_ring(slot);
    hdr->magic = LOG_RING_MAGIC;
    hdr->pd_id = pd_id;
    hdr->head  = 0;
    hdr->tail  = 0;

    get_or_create_ring_state(pd_id);

    uart_puts("\033[32m[+] log_drain: ");
    uart_puts(pd_name_for(pd_id));
    uart_puts(" registered\033[0m\n");
}

/* ── IPC handlers ────────────────────────────────────────────────────────────
 *
 * Signature: uint32_t handler(sel4_badge_t, const sel4_msg_t *, sel4_msg_t *, void *)
 *
 * OP_LOG_WRITE request data layout (sel4_msg_t.data[]):
 *   data[0..3]  = slot   (uint32_t, little-endian)
 *   data[4..7]  = pd_id  (uint32_t, little-endian)
 *
 * OP_LOG_WRITE reply data layout:
 *   data[0..3]  = ok / error code (0 = success)
 *
 * OP_LOG_STATUS reply data layout:
 *   data[0..3]  = ring_count   (uint32_t)
 *   data[4..11] = total_bytes  (uint64_t, little-endian)
 */

static uint32_t handle_log_write(sel4_badge_t badge,
                                  const sel4_msg_t *req,
                                  sel4_msg_t *rep,
                                  void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t slot  = data_rd32(req->data, 0);
    uint32_t pd_id = data_rd32(req->data, 4);

    rep->length = 4;

    if (slot >= MAX_LOG_RINGS) {
        data_wr32(rep->data, 0, 1u); /* LOG_DRAIN_ERR_BAD_SLOT */
        return SEL4_ERR_BAD_ARG;
    }

    if (!log_drain_rings_vaddr) {
        data_wr32(rep->data, 0, 3u); /* LOG_DRAIN_ERR_NOT_MAPPED */
        return SEL4_ERR_BAD_ARG;
    }

    volatile ld_ring_header_t *hdr = get_ring(slot);
    if (hdr->magic != LOG_RING_MAGIC) {
        register_ring(slot, pd_id);
    } else {
        /*
         * Ring header was already initialised (e.g. by the calling PD before
         * the first IPC, or by a previous log_drain_test_init pass).  Ensure
         * we have a ring_state entry for this pd_id even when we skip the
         * full register_ring path.
         */
        get_or_create_ring_state(pd_id);
    }

    log_ring_state_t *rs = find_ring_state(pd_id);
    if (rs) drain_ring(slot, rs);

    data_wr32(rep->data, 0, 0u); /* ok */
    return SEL4_ERR_OK;
}

static uint32_t handle_log_status(sel4_badge_t badge,
                                   const sel4_msg_t *req,
                                   sel4_msg_t *rep,
                                   void *ctx)
{
    (void)badge; (void)req; (void)ctx;

    data_wr32(rep->data, 0, ring_count);
    data_wr64(rep->data, 4, total_bytes);
    rep->length = 12;
    return SEL4_ERR_OK;
}

/* ── Nameserver self-registration ───────────────────────────────────────────── */

static void register_with_nameserver(seL4_CPtr ns_ep)
{
    if (!ns_ep) return;

    /*
     * OP_NS_REGISTER request data layout (matching nameserver.c handle_register):
     *   data[0..3]   = channel_id  (use 0 — log_drain does not have a static channel)
     *   data[4..7]   = pd_id       (13 = log_drain TRACE_PD)
     *   data[8..11]  = cap_classes (0 — log_drain is a system utility, no cap class)
     *   data[12..15] = version     (1)
     *   data[16..47] = name        (NS_NAME_MAX = 32 bytes)
     */
    sel4_msg_t req, rep;
    req.opcode = OP_NS_REGISTER;

    data_wr32(req.data,  0, 0u);    /* channel_id */
    data_wr32(req.data,  4, 13u);   /* pd_id = TRACE_PD_LOG_DRAIN */
    data_wr32(req.data,  8, 0u);    /* cap_classes */
    data_wr32(req.data, 12, 1u);    /* version */

    /* Copy service name "log_drain" into data[16..47] */
    const char *name = "log_drain";
    int i = 0;
    for (; name[i] && (16 + i) < 48; i++)
        req.data[16 + i] = (uint8_t)name[i];
    for (; (16 + i) < 48; i++)
        req.data[16 + i] = 0;

    req.length = 48;

    sel4_call(ns_ep, &req, &rep);
    /* Ignore return value — if nameserver is not yet up, continue anyway */
}

/* ── Test-host entry points ─────────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST

/*
 * log_drain_test_init — reset all state and register handlers.
 *
 * Called by the test harness before each group of tests.  Requires
 * log_drain_rings_vaddr to be set by the test to a valid buffer.
 */
void log_drain_test_init(void)
{
    ring_count      = 0;
    total_bytes     = 0;
    serial_slot     = (uint32_t)-1;
    serial_ready    = false;
    g_serial_ep     = 0;

    for (uint32_t i = 0; i < MAX_LOG_RINGS; i++) {
        ring_states[i].active      = false;
        ring_states[i].pd_id       = 0;
        ring_states[i].bytes_total = 0;
    }

    if (log_drain_rings_vaddr) init_rings();

    sel4_server_init(&g_srv, 0 /* ep unused in tests */);
    sel4_server_register(&g_srv, OP_LOG_WRITE,  handle_log_write,  (void *)0);
    sel4_server_register(&g_srv, OP_LOG_STATUS, handle_log_status, (void *)0);
}

/*
 * log_drain_dispatch_one — exercise one IPC round-trip through the
 * sel4_server dispatch machinery without seL4.
 */
uint32_t log_drain_dispatch_one(sel4_badge_t badge,
                                 const sel4_msg_t *req,
                                 sel4_msg_t *rep)
{
    return sel4_server_dispatch(&g_srv, badge, req, rep);
}

#else /* !AGENTOS_TEST_HOST — production build */

/*
 * log_drain_main — production entry point called by the root task boot
 * dispatcher.
 *
 * my_ep:     listen endpoint capability (seL4 endpoint cap slot).
 * ns_ep:     nameserver endpoint (0 = nameserver not yet available).
 * serial_ep: serial_pd endpoint for UART output (0 = debug-only mode).
 *
 * This function NEVER RETURNS.
 */
void log_drain_main(seL4_CPtr my_ep, seL4_CPtr ns_ep, seL4_CPtr serial_ep)
{
    g_serial_ep = serial_ep;

    ring_count  = 0;
    total_bytes = 0;
    for (uint32_t i = 0; i < MAX_LOG_RINGS; i++) {
        ring_states[i].active      = false;
        ring_states[i].pd_id       = 0;
        ring_states[i].bytes_total = 0;
    }

    if (log_drain_rings_vaddr)
        init_rings();

    uart_puts("[log_drain] starting — agentOS log drain\n");

    /* Self-register with nameserver so other PDs can locate us */
    register_with_nameserver(ns_ep);

    uart_puts("[log_drain] ready — waiting for PD ring registrations\n");

    sel4_server_init(&g_srv, my_ep);
    sel4_server_register(&g_srv, OP_LOG_WRITE,  handle_log_write,  (void *)0);
    sel4_server_register(&g_srv, OP_LOG_STATUS, handle_log_status, (void *)0);

    /* Enter the recv/dispatch/reply loop — never returns */
    sel4_server_run(&g_srv);
}

#endif /* AGENTOS_TEST_HOST */
