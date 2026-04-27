/*
 * agentOS Worker Protection Domain — E5-S5: raw seL4 IPC
 *
 * One binary, N instances (worker_0..worker_7).  The worker index is
 * passed by the root task boot dispatcher in the third argument to
 * worker_main().  Workers register as "worker_N" with the nameserver,
 * then enter the server loop accepting task assignments.
 *
 * Entry point:
 *   void worker_main(seL4_CPtr my_ep, seL4_CPtr ns_ep, uint32_t worker_index)
 *
 * Outbound service dependencies (resolved via nameserver, cached):
 *   "event_bus"  — EventBus pub/sub backbone
 *   "agentfs"    — object store
 *   "vfs_server" — VFS (file I/O)
 *   "net_server" — network service
 *
 * Lifecycle:
 *   boot → register → IDLE (seL4_Recv) → task_assigned → run → done → IDLE
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
#include "sel4_client.h"  /* sel4_client_connect, sel4_client_call */
#include <sel4/sel4.h>    /* seL4_DebugPutChar */

#endif /* AGENTOS_TEST_HOST */

/* ── Contract opcodes ────────────────────────────────────────────────────── */

#ifndef OP_WORKER_TASK_ASSIGN
#define OP_WORKER_TASK_ASSIGN   0x0701u  /* controller → worker: new task */
#endif
#ifndef OP_WORKER_TASK_COMPLETE
#define OP_WORKER_TASK_COMPLETE 0x0702u  /* worker reports completion */
#endif
#ifndef OP_WORKER_STATUS
#define OP_WORKER_STATUS        0x0703u  /* query worker state */
#endif
#ifndef OP_NS_REGISTER
#define OP_NS_REGISTER          0xD0u
#endif
#ifndef NS_NAME_MAX
#define NS_NAME_MAX             32
#endif

/* ── Worker state ────────────────────────────────────────────────────────── */

static struct {
    uint32_t worker_index;
    uint64_t current_task_id;
    uint32_t tasks_completed;
    bool     running;
} g_worker = { 0, 0, 0, false };

/* Cached outbound endpoint caps */
static seL4_CPtr g_eventbus_ep   = 0;
static seL4_CPtr g_agentfs_ep    = 0;
static seL4_CPtr g_vfs_ep        = 0;
static seL4_CPtr g_net_ep        = 0;

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

static inline uint64_t data_rd64(const uint8_t *d, int off)
{
    return (uint64_t)data_rd32(d, off)
         | ((uint64_t)data_rd32(d, off + 4) << 32);
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

/* ── Debug output ────────────────────────────────────────────────────────── */

static void dbg_puts(const char *s)
{
#ifdef CONFIG_PRINTING
    for (; *s; s++)
        seL4_DebugPutChar(*s);
#else
    (void)s;
#endif
}

/* ── Nameserver registration ─────────────────────────────────────────────── */

static void register_with_nameserver(seL4_CPtr ns_ep, uint32_t worker_index)
{
    if (!ns_ep) return;

    sel4_msg_t req, rep;
    req.opcode = OP_NS_REGISTER;
    /*
     * data layout for OP_NS_REGISTER:
     *   data[0..3]   = channel_id   (0)
     *   data[4..7]   = pd_id        (TRACE_PD_WORKER_0 + worker_index = 3 + N)
     *   data[8..11]  = cap_classes  (0)
     *   data[12..15] = version      (1)
     *   data[16..47] = name         ("worker_N\0", NUL-padded)
     */
    data_wr32(req.data, 0,  0u);
    data_wr32(req.data, 4,  3u + worker_index); /* TRACE_PD_WORKER_0 = 3 */
    data_wr32(req.data, 8,  0u);
    data_wr32(req.data, 12, 1u);

    /* Build "worker_N" name */
    const char prefix[] = "worker_";
    int pi = 0;
    for (; prefix[pi]; pi++)
        req.data[16 + pi] = (uint8_t)prefix[pi];
    req.data[16 + pi++] = (uint8_t)('0' + (worker_index & 0xFu));
    for (int ni = pi; ni < NS_NAME_MAX; ni++)
        req.data[16 + ni] = 0;
    req.length = 48u;

    sel4_call(ns_ep, &req, &rep);
}

/* ── Nameserver lookup helper ────────────────────────────────────────────── */

static seL4_CPtr lookup_service(seL4_CPtr ns_ep, const char *svc_name)
{
    if (!ns_ep) return 0;
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = 0xD1u; /* OP_NS_LOOKUP */
    for (int i = 0; i < NS_NAME_MAX; i++)
        req.data[i] = (uint8_t)(svc_name[i] ? svc_name[i] : 0);
    req.length = NS_NAME_MAX;
    sel4_call(ns_ep, &req, &rep);
    if (rep.opcode != 0u) return 0;
    return (seL4_CPtr)data_rd32(rep.data, 0);
}

/* ── IPC handlers ────────────────────────────────────────────────────────── */

/*
 * handle_task_assign — OP_WORKER_TASK_ASSIGN (0x0701)
 *
 * Request:
 *   data[0..7]  = task_id   (uint64)
 *   data[8..11] = task_type (uint32, reserved)
 *
 * Reply:
 *   data[0..3]  = status      (0 = accepted)
 *   data[4..7]  = worker_index (uint32)
 */
static uint32_t handle_task_assign(sel4_badge_t badge, const sel4_msg_t *req,
                                    sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint64_t task_id = data_rd64(req->data, 0);

    if (g_worker.running) {
        /* Already busy — reject */
        data_wr32(rep->data, 0, 6u);        /* SEL4_ERR_BUSY */
        data_wr32(rep->data, 4, g_worker.worker_index);
        rep->length = 8u;
        return SEL4_ERR_INVALID_OP;
    }

    g_worker.current_task_id = task_id;
    g_worker.running         = true;

    dbg_puts("[worker] Task assigned\n");

    /*
     * Execute the task synchronously in this handler for MVP.
     * Real implementation: dispatch to a worker thread or co-routine.
     */
    g_worker.tasks_completed++;
    g_worker.running = false;

    data_wr32(rep->data, 0, 0u); /* OK */
    data_wr32(rep->data, 4, g_worker.worker_index);
    data_wr64(rep->data, 8, task_id);
    rep->length = 16u;
    return SEL4_ERR_OK;
}

/*
 * handle_status — OP_WORKER_STATUS (0x0703)
 *
 * Reply:
 *   data[0..3]   = worker_index     (uint32)
 *   data[4..7]   = tasks_completed  (uint32)
 *   data[8..15]  = current_task_id  (uint64)
 *   data[16..19] = running          (uint32, 0 or 1)
 */
static uint32_t handle_status(sel4_badge_t badge, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;

    data_wr32(rep->data, 0,  g_worker.worker_index);
    data_wr32(rep->data, 4,  g_worker.tasks_completed);
    data_wr64(rep->data, 8,  g_worker.current_task_id);
    data_wr32(rep->data, 16, g_worker.running ? 1u : 0u);
    rep->length = 20u;
    return SEL4_ERR_OK;
}

/* ── Test-visible helpers ────────────────────────────────────────────────── */

static void worker_test_init(uint32_t index)
{
    g_worker.worker_index    = index;
    g_worker.current_task_id = 0;
    g_worker.tasks_completed = 0;
    g_worker.running         = false;
    g_eventbus_ep            = 0;
    g_agentfs_ep             = 0;
    g_vfs_ep                 = 0;
    g_net_ep                 = 0;
    sel4_server_init(&g_srv, 0);
    sel4_server_register(&g_srv, OP_WORKER_TASK_ASSIGN,  handle_task_assign, NULL);
    sel4_server_register(&g_srv, OP_WORKER_STATUS,       handle_status,      NULL);
}

static uint32_t worker_dispatch_one(sel4_badge_t badge,
                                     const sel4_msg_t *req,
                                     sel4_msg_t *rep)
{
    return sel4_server_dispatch(&g_srv, badge, req, rep);
}

static uint32_t worker_get_tasks_completed(void)
{
    return g_worker.tasks_completed;
}

static uint32_t worker_get_index(void)
{
    return g_worker.worker_index;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

#ifndef AGENTOS_TEST_HOST
void worker_main(seL4_CPtr my_ep, seL4_CPtr ns_ep, uint32_t worker_index)
{
    g_worker.worker_index = worker_index;

    dbg_puts("[worker] Starting up\n");

    /* Register with nameserver as "worker_N" */
    register_with_nameserver(ns_ep, worker_index);

    /* Cache outbound endpoint caps (resolved once, reused thereafter) */
    g_eventbus_ep = lookup_service(ns_ep, "event_bus");
    g_agentfs_ep  = lookup_service(ns_ep, "agentfs");
    g_vfs_ep      = lookup_service(ns_ep, "vfs_server");
    g_net_ep      = lookup_service(ns_ep, "net_server");

    dbg_puts("[worker] Entering server loop\n");

    sel4_server_init(&g_srv, my_ep);
    sel4_server_register(&g_srv, OP_WORKER_TASK_ASSIGN, handle_task_assign, NULL);
    sel4_server_register(&g_srv, OP_WORKER_STATUS,      handle_status,      NULL);

    /* Enter server loop — never returns */
    sel4_server_run(&g_srv);
}

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep) { worker_main(my_ep, ns_ep, 0u); }

#endif /* !AGENTOS_TEST_HOST */
