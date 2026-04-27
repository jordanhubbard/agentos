/*
 * agentOS InitAgent Protection Domain — E5-S5: raw seL4 IPC
 *
 * Priority 100. First real agent. Bootstraps the agent ecosystem.
 * Subscribes to EventBus, registers with nameserver, prints the boot banner,
 * and serves as the dynamic spawn broker.
 *
 * Entry point:
 *   void init_agent_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
 *
 * Outbound service dependencies (resolved via nameserver):
 *   "event_bus"    — EventBus pub/sub backbone
 *   "quota_pd"     — per-agent resource quota enforcement
 *   "mem_profiler" — memory usage monitoring
 *   "net_isolator" — per-agent network ACL
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* ── Conditional compilation ─────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST
/*
 * Host-side test build: minimal type stubs — no seL4 or Microkit headers.
 * framework.h (included by the test file before this unit) provides the
 * mock MR layer.
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

/* sel4_call stub: records the call but does nothing in test builds */
static sel4_msg_t _test_last_call_rep;
static inline void sel4_call(seL4_CPtr ep, const sel4_msg_t *req, sel4_msg_t *rep)
{
    (void)ep; (void)req;
    /* Return the pre-set stub reply */
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

/* ── Contract opcodes (guarded so test overrides are possible) ──────────── */

#ifndef MSG_INITAGENT_STATUS
#define MSG_INITAGENT_STATUS    0x0302u
#endif
#ifndef MSG_SPAWN_AGENT
#define MSG_SPAWN_AGENT         0x0801u
#endif
#ifndef MSG_SPAWN_AGENT_REPLY
#define MSG_SPAWN_AGENT_REPLY   0x0802u
#endif
#ifndef OP_EVENTBUS_SUBSCRIBE
#define OP_EVENTBUS_SUBSCRIBE   0x0002u
#endif
#ifndef OP_EVENTBUS_STATUS
#define OP_EVENTBUS_STATUS      0x0004u
#endif
#ifndef OP_QUOTA_REGISTER
#define OP_QUOTA_REGISTER       0x60u
#endif
#ifndef OP_QUOTA_TICK
#define OP_QUOTA_TICK           0x61u
#endif
#ifndef OP_MEM_REGISTER
#define OP_MEM_REGISTER         0xC0u
#endif
#ifndef OP_NET_ACL_CLEAR
#define OP_NET_ACL_CLEAR        0x73u
#endif
#ifndef OP_NS_REGISTER
#define OP_NS_REGISTER          0xD0u
#endif
#ifndef NS_NAME_MAX
#define NS_NAME_MAX             32
#endif
#ifndef QUOTA_FLAG_REVOKED
#define QUOTA_FLAG_REVOKED      (1u << 3)
#endif

/* ── Module constants ────────────────────────────────────────────────────── */

#define MAX_PENDING_SPAWNS    8
#define SPAWN_PENDING_FLAG    0x80000000u
#define DEFAULT_CPU_QUOTA_MS  5000u
#define DEFAULT_MEM_QUOTA_KB  4096u

/* ── Spawn pending table ─────────────────────────────────────────────────── */

typedef struct {
    uint32_t spawn_id;
    uint64_t hash_lo;
    uint64_t hash_hi;
    uint32_t priority;
    bool     pending;
    int32_t  slot_id;
    bool     quota_registered;
    uint32_t quota_cpu_ms;
    uint32_t quota_mem_kb;
} spawn_req_t;

static spawn_req_t spawn_table[MAX_PENDING_SPAWNS];
static uint32_t    spawn_seq = 0;

/* ── Module state ────────────────────────────────────────────────────────── */

static struct {
    bool     started;
    bool     eventbus_subscribed;
    uint32_t event_count;
    uint32_t query_count;
    uint32_t spawn_count;
} g_state = { false, false, 0, 0, 0 };

/* Cached outbound endpoint caps (filled by nameserver lookup at startup) */
static seL4_CPtr g_eventbus_ep    = 0;
static seL4_CPtr g_quota_ep       = 0;
static seL4_CPtr g_mem_profiler_ep = 0;
static seL4_CPtr g_net_isolator_ep = 0;

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

/* ── Spawn table helpers ──────────────────────────────────────────────────── */

static void spawn_table_init(void)
{
    for (int i = 0; i < MAX_PENDING_SPAWNS; i++) {
        spawn_table[i].pending           = false;
        spawn_table[i].spawn_id          = 0;
        spawn_table[i].slot_id           = -1;
        spawn_table[i].quota_registered  = false;
        spawn_table[i].quota_cpu_ms      = 0;
        spawn_table[i].quota_mem_kb      = 0;
    }
}

static int spawn_table_alloc(void)
{
    for (int i = 0; i < MAX_PENDING_SPAWNS; i++) {
        if (!spawn_table[i].pending) return i;
    }
    return -1;
}

static int spawn_table_find(uint32_t spawn_id)
{
    for (int i = 0; i < MAX_PENDING_SPAWNS; i++) {
        if (spawn_table[i].pending && spawn_table[i].spawn_id == spawn_id)
            return i;
    }
    return -1;
}

/* ── Nameserver registration ─────────────────────────────────────────────── */

static void register_with_nameserver(seL4_CPtr ns_ep)
{
    if (!ns_ep) return;

    sel4_msg_t req, rep;
    req.opcode = OP_NS_REGISTER;
    /*
     * data layout for OP_NS_REGISTER (mirrors nameserver.h convention):
     *   data[0..3]   = channel_id   (0 — nameserver-allocated)
     *   data[4..7]   = pd_id        (TRACE_PD_INIT_AGENT = 2)
     *   data[8..11]  = cap_classes  (0)
     *   data[12..15] = version      (1)
     *   data[16..47] = name         ("init_agent\0", NUL-padded)
     */
    data_wr32(req.data, 0,  0u);   /* channel_id */
    data_wr32(req.data, 4,  2u);   /* TRACE_PD_INIT_AGENT */
    data_wr32(req.data, 8,  0u);   /* cap_classes */
    data_wr32(req.data, 12, 1u);   /* version */
    const char name[] = "init_agent";
    for (int i = 0; i < NS_NAME_MAX; i++)
        req.data[16 + i] = (uint8_t)(i < (int)(sizeof(name) - 1) ? name[i] : 0);
    req.length = 48u;

    sel4_call(ns_ep, &req, &rep);
    /* Ignore result — nameserver offline is non-fatal at boot */
}

/* ── Nameserver service lookup helper ────────────────────────────────────── */

static seL4_CPtr lookup_service(seL4_CPtr ns_ep, const char *svc_name)
{
    if (!ns_ep) return 0;

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = 0xD1u; /* OP_NS_LOOKUP */
    for (int i = 0; i < NS_NAME_MAX; i++)
        req.data[i] = (uint8_t)(svc_name[i] ? svc_name[i] : 0);
    req.length = NS_NAME_MAX;
    sel4_call(ns_ep, &req, &rep);
    if (rep.opcode != 0u) return 0; /* NS_OK == 0 */
    /* MR1 = channel_id in the nameserver reply.  On raw seL4 this becomes
     * the endpoint cap slot delivered by the nameserver via cap transfer.
     * In the current implementation the channel_id IS the seL4 cap slot. */
    return (seL4_CPtr)data_rd32(rep.data, 0);
}

/* ── Quota helpers ───────────────────────────────────────────────────────── */

static bool quota_register_agent(uint32_t agent_id, uint32_t cpu_ms, uint32_t mem_kb)
{
    if (!g_quota_ep) return false;
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_QUOTA_REGISTER;
    data_wr32(req.data, 0, agent_id);
    data_wr32(req.data, 4, cpu_ms);
    data_wr32(req.data, 8, mem_kb);
    req.length = 12u;
    sel4_call(g_quota_ep, &req, &rep);
    return (rep.opcode == SEL4_ERR_OK);
}

static uint32_t quota_tick_agent(uint32_t agent_id, uint32_t cpu_delta_ms,
                                  uint32_t mem_cur_kb)
{
    if (!g_quota_ep) return 0;
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_QUOTA_TICK;
    data_wr32(req.data, 0, agent_id);
    data_wr32(req.data, 4, cpu_delta_ms);
    data_wr32(req.data, 8, mem_cur_kb);
    req.length = 12u;
    sel4_call(g_quota_ep, &req, &rep);
    if (rep.opcode != SEL4_ERR_OK) return 0;
    return data_rd32(rep.data, 0); /* quota flags */
}

static void quota_tick_all_agents(void)
{
    for (int i = 0; i < MAX_PENDING_SPAWNS; i++) {
        if (!spawn_table[i].quota_registered) continue;
        if (spawn_table[i].pending) continue;

        uint32_t flags = quota_tick_agent(spawn_table[i].spawn_id, 1u, 0u);

        if (flags & QUOTA_FLAG_REVOKED) {
            dbg_puts("[init_agent] quota revoked for agent\n");
            spawn_table[i].quota_registered = false;

            /* Clear net_isolator ACL for this agent slot */
            if (g_net_isolator_ep) {
                sel4_msg_t req = {0}, rep = {0};
                req.opcode = OP_NET_ACL_CLEAR;
                data_wr32(req.data, 0, spawn_table[i].spawn_id);
                req.length = 4u;
                sel4_call(g_net_isolator_ep, &req, &rep);
            }
        }
    }
}

/* ── EventBus helpers ────────────────────────────────────────────────────── */

static void subscribe_to_eventbus(void)
{
    if (!g_eventbus_ep) {
        dbg_puts("[init_agent] EventBus endpoint not resolved\n");
        return;
    }
    dbg_puts("[init_agent] Subscribing to EventBus...\n");

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_EVENTBUS_SUBSCRIBE;
    data_wr64(req.data, 0, (uint64_t)0u);  /* ntfn_cap badge token (0 = polling mode) */
    data_wr32(req.data, 8, 0u);            /* topic_mask (0 = all events) */
    req.length = 12u;
    sel4_call(g_eventbus_ep, &req, &rep);

    if (rep.opcode == SEL4_ERR_OK) {
        g_state.eventbus_subscribed = true;
        dbg_puts("[init_agent] EventBus subscription: OK\n");
    } else {
        dbg_puts("[init_agent] EventBus subscription: FAILED\n");
    }
}

static void query_eventbus_status(void)
{
    g_state.query_count++;
    if (!g_eventbus_ep) return;

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_EVENTBUS_STATUS;
    req.length = 0u;
    sel4_call(g_eventbus_ep, &req, &rep);

    if (rep.opcode == SEL4_ERR_OK) {
        uint64_t head = data_rd64(rep.data, 0);
        g_state.event_count = (uint32_t)head;
    }
}

/* ── EventBus publish helper ─────────────────────────────────────────────── */

static void publish_spawn_event(uint32_t spawn_id, int32_t slot_id,
                                 uint64_t hash_lo, uint64_t hash_hi)
{
    if (!g_eventbus_ep) return;
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = 0x0006u; /* MSG_EVENTBUS_PUBLISH */
    data_wr32(req.data, 0, 0x0401u); /* MSG_EVENT_AGENT_SPAWNED */
    data_wr32(req.data, 4, spawn_id);
    data_wr32(req.data, 8, (uint32_t)slot_id);
    data_wr32(req.data, 12, (uint32_t)(hash_lo & 0xFFFFFFFFu));
    data_wr32(req.data, 16, (uint32_t)(hash_hi & 0xFFFFFFFFu));
    req.length = 20u;
    sel4_call(g_eventbus_ep, &req, &rep);
}

/* ── Spawn broker helpers ────────────────────────────────────────────────── */

static void handle_spawn_reply(uint32_t spawn_id, int32_t slot_id)
{
    int tbl = spawn_table_find(spawn_id);
    if (tbl < 0) {
        dbg_puts("[init_agent] SPAWN_REPLY: unknown spawn_id\n");
        return;
    }

    spawn_table[tbl].slot_id = slot_id;
    spawn_table[tbl].pending = (slot_id < 0);

    if (slot_id >= 0) {
        g_state.spawn_count++;
        bool quota_ok = quota_register_agent(spawn_id,
                                              DEFAULT_CPU_QUOTA_MS,
                                              DEFAULT_MEM_QUOTA_KB);
        spawn_table[tbl].quota_registered = quota_ok;
        spawn_table[tbl].quota_cpu_ms     = DEFAULT_CPU_QUOTA_MS;
        spawn_table[tbl].quota_mem_kb     = DEFAULT_MEM_QUOTA_KB;

        /* Register with memory profiler */
        if (g_mem_profiler_ep) {
            sel4_msg_t req = {0}, rep = {0};
            req.opcode = OP_MEM_REGISTER;
            data_wr32(req.data, 0, spawn_id);
            req.length = 4u;
            sel4_call(g_mem_profiler_ep, &req, &rep);
        }

        /* Clear net_isolator ACL (deny-all default) */
        if (g_net_isolator_ep) {
            sel4_msg_t req = {0}, rep = {0};
            req.opcode = OP_NET_ACL_CLEAR;
            data_wr32(req.data, 0, spawn_id);
            req.length = 4u;
            sel4_call(g_net_isolator_ep, &req, &rep);
        }

        publish_spawn_event(spawn_id, slot_id,
                            spawn_table[tbl].hash_lo,
                            spawn_table[tbl].hash_hi);
        spawn_table[tbl].pending = false;
    } else {
        dbg_puts("[init_agent] SPAWN_REPLY: controller reported failure\n");
        spawn_table[tbl].pending           = false;
        spawn_table[tbl].quota_registered  = false;
    }
}

/* ── IPC handlers ────────────────────────────────────────────────────────── */

/*
 * handle_status — MSG_INITAGENT_STATUS (0x0302)
 *
 * Reply:
 *   data[0..3]  = event_count  (uint32)
 *   data[4..7]  = eventbus_subscribed (uint32, 0 or 1)
 *   data[8..11] = spawn_count  (uint32)
 */
static uint32_t handle_status(sel4_badge_t badge, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;

    data_wr32(rep->data, 0, g_state.event_count);
    data_wr32(rep->data, 4, g_state.eventbus_subscribed ? 1u : 0u);
    data_wr32(rep->data, 8, g_state.spawn_count);
    rep->length = 12u;
    return SEL4_ERR_OK;
}

/*
 * handle_spawn_agent — MSG_SPAWN_AGENT (0x0801)
 *
 * Request:
 *   data[0..7]   = hash_lo  (uint64)
 *   data[8..15]  = hash_hi  (uint64)
 *   data[16..19] = priority (uint32, 0 = use default)
 *
 * Reply:
 *   data[0..3]  = spawn_id | SPAWN_PENDING_FLAG  (uint32)
 *   data[4..7]  = status (0 = queued)
 */
static uint32_t handle_spawn_agent(sel4_badge_t badge, const sel4_msg_t *req,
                                    sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint64_t hash_lo  = data_rd64(req->data, 0);
    uint64_t hash_hi  = data_rd64(req->data, 8);
    uint32_t priority = data_rd32(req->data, 16);
    if (priority == 0u) priority = 80u; /* PRIO_COMPUTE default */

    int tbl = spawn_table_alloc();
    if (tbl < 0) {
        dbg_puts("[init_agent] SPAWN_AGENT: table full\n");
        data_wr32(rep->data, 0, 0u);
        data_wr32(rep->data, 4, 0xE1u); /* ERR_SPAWN_TABLE_FULL */
        rep->length = 8u;
        return SEL4_ERR_NO_MEM;
    }

    uint32_t spawn_id = (++spawn_seq) & 0x7FFFFFFFu;
    spawn_table[tbl].spawn_id  = spawn_id;
    spawn_table[tbl].hash_lo   = hash_lo;
    spawn_table[tbl].hash_hi   = hash_hi;
    spawn_table[tbl].priority  = priority;
    spawn_table[tbl].pending   = true;
    spawn_table[tbl].slot_id   = -1;

    dbg_puts("[init_agent] SPAWN_AGENT queued\n");

    data_wr32(rep->data, 0, spawn_id | SPAWN_PENDING_FLAG);
    data_wr32(rep->data, 4, 0u); /* status: OK (queued) */
    rep->length = 8u;
    return SEL4_ERR_OK;
}

/*
 * handle_spawn_reply_notification — MSG_SPAWN_AGENT_REPLY (0x0802)
 *
 * Sent by the controller (or root task) to inform init_agent that a
 * pending spawn request has completed.
 *
 * Request:
 *   data[0..3]  = spawn_id  (uint32)
 *   data[4..7]  = slot_id   (int32, negative = failure)
 *
 * Reply:
 *   (empty — status in opcode field)
 */
static uint32_t handle_spawn_complete(sel4_badge_t badge, const sel4_msg_t *req,
                                       sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t spawn_id = data_rd32(req->data, 0);
    int32_t  slot_id  = (int32_t)data_rd32(req->data, 4);

    handle_spawn_reply(spawn_id, slot_id);

    /* Also run a periodic quota tick pass on each spawn completion */
    quota_tick_all_agents();

    rep->length = 0u;
    return SEL4_ERR_OK;
}

/* ── Test-visible helpers ────────────────────────────────────────────────── */

/*
 * init_agent_test_init — reset all module state for host-side tests.
 * Must be called before any dispatch in test code.
 */
static void init_agent_test_init(void)
{
    spawn_table_init();
    g_state.started             = false;
    g_state.eventbus_subscribed = false;
    g_state.event_count         = 0;
    g_state.query_count         = 0;
    g_state.spawn_count         = 0;
    spawn_seq                   = 0;
    g_eventbus_ep               = 0;
    g_quota_ep                  = 0;
    g_mem_profiler_ep           = 0;
    g_net_isolator_ep           = 0;
    sel4_server_init(&g_srv, 0);
    sel4_server_register(&g_srv, MSG_INITAGENT_STATUS, handle_status, NULL);
    sel4_server_register(&g_srv, MSG_SPAWN_AGENT,      handle_spawn_agent, NULL);
    sel4_server_register(&g_srv, MSG_SPAWN_AGENT_REPLY, handle_spawn_complete, NULL);
}

/*
 * init_agent_dispatch_one — dispatch a single message for testing.
 * Returns the handler return code (also placed in rep->opcode).
 */
static uint32_t init_agent_dispatch_one(sel4_badge_t badge,
                                         const sel4_msg_t *req,
                                         sel4_msg_t *rep)
{
    return sel4_server_dispatch(&g_srv, badge, req, rep);
}

/* Accessor for test assertions */
static bool init_agent_get_eventbus_subscribed(void)
{
    return g_state.eventbus_subscribed;
}

static uint32_t init_agent_get_spawn_count(void) { return g_state.spawn_count; }
static uint32_t init_agent_get_spawn_seq(void)   { return spawn_seq; }

/* ── Entry point ─────────────────────────────────────────────────────────── */

#ifndef AGENTOS_TEST_HOST
void init_agent_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    dbg_puts("[init_agent] Starting up...\n");

    spawn_table_init();

    /* Register ourselves with the nameserver */
    register_with_nameserver(ns_ep);

    /* Resolve outbound service endpoints via nameserver */
    g_eventbus_ep     = lookup_service(ns_ep, "event_bus");
    g_quota_ep        = lookup_service(ns_ep, "quota_pd");
    g_mem_profiler_ep = lookup_service(ns_ep, "mem_profiler");
    g_net_isolator_ep = lookup_service(ns_ep, "net_isolator");

    /* Subscribe to the EventBus */
    subscribe_to_eventbus();

    dbg_puts("[init_agent] Entering server loop.\n");
    g_state.started = true;

    /* Register opcodes */
    sel4_server_init(&g_srv, my_ep);
    sel4_server_register(&g_srv, MSG_INITAGENT_STATUS,  handle_status,          NULL);
    sel4_server_register(&g_srv, MSG_SPAWN_AGENT,       handle_spawn_agent,     NULL);
    sel4_server_register(&g_srv, MSG_SPAWN_AGENT_REPLY, handle_spawn_complete,  NULL);

    /* Enter server loop — never returns */
    sel4_server_run(&g_srv);
}

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep) { init_agent_main(my_ep, ns_ep); }

#endif /* !AGENTOS_TEST_HOST */
