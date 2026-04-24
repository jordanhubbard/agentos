/*
 * agentOS EventBus Protection Domain — E5-S1: raw seL4 IPC
 *
 * The EventBus is the publish-subscribe backbone of agentOS.
 * It runs as a passive server (only executes when called via seL4 IPC).
 *
 * Implementation:
 *   - Shared ring buffer in memory region 'eventbus_ring'
 *   - Subscribers read directly from the ring (zero-copy)
 *   - Publishers write via IPC (serialized through this PD)
 *   - Subscribers hold seL4 notification capabilities; EventBus
 *     stores the ntfn_cap and calls seL4_Signal(ntfn_cap) on publish.
 *
 * Notification model:
 *   On SUBSCRIBE the caller passes its notification capability badge token
 *   in req->data[0..7] (uint64_t).  EventBus stores that token and uses it
 *   as the notification cap slot when signalling.
 *
 *   NOTE: This is a known simplification — in a full capability-transfer
 *   implementation the caller would pass the notification cap via seL4
 *   cap-transfer IPC.  The badge-token approach is correct for the current
 *   single-machine, root-task-allocated notification layout where the root
 *   task assigns notification caps by fixed slot index (the badge IS the
 *   seL4_CPtr).  Future work: E5-S2 will replace badge tokens with proper
 *   cap transfer once the root task's CNode layout is stable.
 *
 * Entry point:
 *   void event_bus_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
 *
 * Priority: 200 (high — event delivery is latency-sensitive)
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

/* seL4_Signal stub: no-op in test builds */
static inline void seL4_Signal(seL4_CPtr cap) { (void)cap; }

/* seL4_DebugPutChar stub */
static inline void seL4_DebugPutChar(char c) { (void)c; }

#else /* !AGENTOS_TEST_HOST — production build */

#include <stdint.h>
#include <stdbool.h>
#include "sel4_server.h"   /* sel4_server_t, sel4_server_init/register/run */
#include "sel4_client.h"   /* sel4_client_call */
#include "sel4_ipc.h"      /* sel4_msg_t, sel4_badge_t, SEL4_ERR_* */
#include <sel4/sel4.h>     /* seL4_Signal, seL4_DebugPutChar */

#endif /* AGENTOS_TEST_HOST */

/* ── Contract opcodes ──────────────────────────────────────────────────────── */

#ifndef MSG_EVENTBUS_INIT
#define MSG_EVENTBUS_INIT           0x0001u
#endif
#ifndef MSG_EVENTBUS_SUBSCRIBE
#define MSG_EVENTBUS_SUBSCRIBE      0x0002u
#endif
#ifndef MSG_EVENTBUS_UNSUBSCRIBE
#define MSG_EVENTBUS_UNSUBSCRIBE    0x0003u
#endif
#ifndef MSG_EVENTBUS_STATUS
#define MSG_EVENTBUS_STATUS         0x0004u
#endif
#ifndef MSG_EVENTBUS_READY
#define MSG_EVENTBUS_READY          0x0101u
#endif
#ifndef OP_PUBLISH_BATCH
#define OP_PUBLISH_BATCH            0x0025u
#endif
#ifndef PUBLISH_BATCH_MAX
#define PUBLISH_BATCH_MAX           16u
#endif
#ifndef EVENTBUS_ERR_OVERFLOW
#define EVENTBUS_ERR_OVERFLOW       3u
#endif

/* Nameserver opcode */
#ifndef OP_NS_REGISTER
#define OP_NS_REGISTER 0xD0u
#endif
#ifndef NS_OK
#define NS_OK 0u
#endif
#ifndef NS_NAME_MAX
#define NS_NAME_MAX 32
#endif

/* ── Ring buffer layout constants ──────────────────────────────────────────── */

#define AGENTOS_RING_MAGIC          0xA6E70B05u
#define EVENTBUS_RING_SIZE          0x40000u    /* 256 KB */
#define EVENTBUS_BATCH_STAGING_SIZE 768u
#define EVENTBUS_BATCH_STAGING_OFFSET (EVENTBUS_RING_SIZE - EVENTBUS_BATCH_STAGING_SIZE)

/* ── Ring buffer types ─────────────────────────────────────────────────────── */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t capacity;
    uint64_t head;
    uint64_t tail;
    uint32_t overflow_count;
    uint8_t  _pad[36];
} eb_ring_header_t;

typedef struct {
    uint64_t seq;
    uint64_t timestamp_ns;
    uint32_t kind;
    uint32_t source_pd;
    uint32_t payload_len;
    uint8_t  payload[64];
} eb_event_t;

typedef struct {
    uint16_t topic_len;
    uint16_t payload_len;
    char     data[];
} eb_batch_event_t;

#define EB_BATCH_STRIDE(e) \
    (((uint32_t)(sizeof(eb_batch_event_t) + (e)->topic_len + (e)->payload_len) + 3u) & ~3u)

/* ── Subscriber table ─────────────────────────────────────────────────────── */

#define MAX_SUBSCRIBERS 64

typedef struct {
    bool      active;
    seL4_CPtr ntfn_cap;     /* notification capability (badge token == seL4_CPtr) */
    uint32_t  topic_mask;   /* event kind bitmask (0 = all events) */
    uint64_t  last_seq;     /* last seen sequence number */
} subscriber_t;

/* ── Module state ─────────────────────────────────────────────────────────── */

/*
 * eventbus_ring_vaddr — base virtual address of the shared event ring region.
 * In production this is set by the root task before calling event_bus_main().
 * In test builds it is set directly by the test harness.
 */
uintptr_t eventbus_ring_vaddr;

static subscriber_t subscribers[MAX_SUBSCRIBERS];
static uint32_t     sub_count = 0;
static uint64_t     event_seq = 0;
static bool         ring_initialized = false;

/* Server instance */
static sel4_server_t g_srv;

#define EVENTBUS_RING ((volatile eb_ring_header_t *)eventbus_ring_vaddr)

/* ── Data-field helpers (little-endian in sel4_msg_t.data[]) ─────────────── */

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

/* ── Debug output ─────────────────────────────────────────────────────────── */

static void dbg_puts(const char *s)
{
    for (; *s; s++)
        seL4_DebugPutChar(*s);
}

/* ── Ring buffer operations ─────────────────────────────────────────────────── */

static void eventbus_init_ring(void)
{
    volatile eb_ring_header_t *ring = EVENTBUS_RING;
    ring->magic          = AGENTOS_RING_MAGIC;
    ring->version        = 1;
    ring->capacity       = (EVENTBUS_BATCH_STAGING_OFFSET - sizeof(eb_ring_header_t))
                           / sizeof(eb_event_t);
    ring->head           = 0;
    ring->tail           = 0;
    ring->overflow_count = 0;
    ring_initialized     = true;
}

/*
 * eventbus_write — write one event to the ring.
 *
 * Returns SEL4_ERR_OK on success, SEL4_ERR_NO_MEM if the ring is full.
 */
static uint32_t eventbus_write(uint32_t kind, uint32_t source_pd,
                                const uint8_t *payload, uint32_t payload_len)
{
    if (!eventbus_ring_vaddr || !ring_initialized)
        return SEL4_ERR_BAD_ARG;

    volatile eb_ring_header_t *ring = EVENTBUS_RING;
    volatile eb_event_t *events = (volatile eb_event_t *)
        ((uint8_t *)EVENTBUS_RING + sizeof(eb_ring_header_t));

    uint64_t next_head = (ring->head + 1u) % ring->capacity;
    if (next_head == ring->tail) {
        ring->overflow_count++;
        dbg_puts("[event_bus] ring full, dropping event\n");
        return SEL4_ERR_NO_MEM;
    }

    volatile eb_event_t *slot = &events[ring->head];
    slot->seq         = event_seq++;
    slot->kind        = kind;
    slot->source_pd   = source_pd;
    slot->payload_len = payload_len < 64u ? payload_len : 64u;
    slot->timestamp_ns = 0;  /* filled by caller if needed; 0 acceptable */

    for (uint32_t i = 0; i < slot->payload_len; i++)
        slot->payload[i] = payload ? payload[i] : 0u;

    __asm__ volatile("" ::: "memory");
    ring->head = next_head;
    return SEL4_ERR_OK;
}

/* ── Subscriber notifications ─────────────────────────────────────────────── */

static void eventbus_notify_all(void)
{
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (subscribers[i].active && subscribers[i].ntfn_cap != 0)
            seL4_Signal(subscribers[i].ntfn_cap);
    }
}

/* ── IPC handlers ─────────────────────────────────────────────────────────── */

/*
 * handle_init — MSG_EVENTBUS_INIT (0x0001)
 *
 * Request: (no data required)
 * Reply:
 *   data[0..3]  = MSG_EVENTBUS_READY (uint32)
 *   data[4..11] = capacity (uint64)
 */
static uint32_t handle_init(sel4_badge_t badge,
                             const sel4_msg_t *req,
                             sel4_msg_t *rep,
                             void *ctx)
{
    (void)badge; (void)req; (void)ctx;

    dbg_puts("[event_bus] init from caller\n");

    if (!eventbus_ring_vaddr) {
        rep->length = 0;
        return SEL4_ERR_BAD_ARG;
    }

    eventbus_init_ring();

    /* Write system-ready event into the ring */
    eventbus_write(0x0403u, 0, NULL, 0);  /* MSG_EVENT_SYSTEM_READY */

    data_wr32(rep->data, 0, MSG_EVENTBUS_READY);
    data_wr64(rep->data, 4, EVENTBUS_RING->capacity);
    rep->length = 12;

    dbg_puts("[event_bus] READY\n");
    return SEL4_ERR_OK;
}

/*
 * handle_subscribe — MSG_EVENTBUS_SUBSCRIBE (0x0002)
 *
 * Request data layout:
 *   data[0..7]  = ntfn_cap (uint64 — badge token identifying notification cap)
 *   data[8..11] = topic_mask (uint32)
 *
 * Reply data layout (on SEL4_ERR_OK):
 *   data[0..3]  = subscriber_id (uint32, index into subscribers[])
 */
static uint32_t handle_subscribe(sel4_badge_t badge,
                                  const sel4_msg_t *req,
                                  sel4_msg_t *rep,
                                  void *ctx)
{
    (void)badge; (void)ctx;

    uint64_t ntfn_cap  = data_rd64(req->data, 0);
    uint32_t topic_mask = data_rd32(req->data, 8);

    rep->length = 4;

    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (!subscribers[i].active) {
            subscribers[i].active     = true;
            subscribers[i].ntfn_cap   = (seL4_CPtr)ntfn_cap;
            subscribers[i].topic_mask = topic_mask;
            subscribers[i].last_seq   = event_seq;
            sub_count++;

            data_wr32(rep->data, 0, (uint32_t)i);
            dbg_puts("[event_bus] subscriber registered\n");
            return SEL4_ERR_OK;
        }
    }

    /* Subscriber table full */
    data_wr32(rep->data, 0, (uint32_t)-1);
    dbg_puts("[event_bus] ERROR: subscriber table full\n");
    return SEL4_ERR_NO_MEM;
}

/*
 * handle_unsubscribe — MSG_EVENTBUS_UNSUBSCRIBE (0x0003)
 *
 * Request data layout:
 *   data[0..3]  = subscriber_id (uint32)
 *
 * Reply: (no data, status in opcode)
 */
static uint32_t handle_unsubscribe(sel4_badge_t badge,
                                    const sel4_msg_t *req,
                                    sel4_msg_t *rep,
                                    void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t handle = data_rd32(req->data, 0);
    rep->length = 0;

    if (handle < (uint32_t)MAX_SUBSCRIBERS && subscribers[handle].active) {
        subscribers[handle].active   = false;
        subscribers[handle].ntfn_cap = 0;
        sub_count--;
        dbg_puts("[event_bus] subscriber removed\n");
        return SEL4_ERR_OK;
    }

    return SEL4_ERR_BAD_ARG;
}

/*
 * handle_status — MSG_EVENTBUS_STATUS (0x0004)
 *
 * Reply data layout:
 *   data[0..7]  = head          (uint64)
 *   data[8..15] = tail          (uint64)
 *   data[16..23] = capacity     (uint64)
 *   data[24..27] = overflow_count (uint32)
 *   data[28..31] = subscriber_count (uint32)
 */
static uint32_t handle_status(sel4_badge_t badge,
                               const sel4_msg_t *req,
                               sel4_msg_t *rep,
                               void *ctx)
{
    (void)badge; (void)req; (void)ctx;

    if (!eventbus_ring_vaddr || !ring_initialized) {
        rep->length = 0;
        return SEL4_ERR_BAD_ARG;
    }

    volatile eb_ring_header_t *ring = EVENTBUS_RING;
    data_wr64(rep->data,  0, ring->head);
    data_wr64(rep->data,  8, ring->tail);
    data_wr64(rep->data, 16, ring->capacity);
    data_wr32(rep->data, 24, ring->overflow_count);
    data_wr32(rep->data, 28, sub_count);
    rep->length = 32;
    return SEL4_ERR_OK;
}

/*
 * handle_publish_batch — OP_PUBLISH_BATCH (0x0025)
 *
 * Request data layout:
 *   data[0..3]  = count  (uint32, number of eb_batch_event_t entries, 1..16)
 *   data[4..7]  = offset (uint32, byte offset from eventbus_ring_vaddr)
 *
 * Reply data layout:
 *   data[0..3]  = dispatched (uint32)
 *   data[4..7]  = dropped    (uint32)
 */
static uint32_t handle_publish_batch(sel4_badge_t badge,
                                      const sel4_msg_t *req,
                                      sel4_msg_t *rep,
                                      void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t count  = data_rd32(req->data, 0);
    uint32_t offset = data_rd32(req->data, 4);

    rep->length = 8;

    if (count == 0 || count > PUBLISH_BATCH_MAX) {
        dbg_puts("[event_bus] BATCH: bad count\n");
        data_wr32(rep->data, 0, 0);
        data_wr32(rep->data, 4, 0);
        return SEL4_ERR_BAD_ARG;
    }

    if (offset >= EVENTBUS_RING_SIZE) {
        dbg_puts("[event_bus] BATCH: offset out of range\n");
        data_wr32(rep->data, 0, 0);
        data_wr32(rep->data, 4, 0);
        return SEL4_ERR_BAD_ARG;
    }

    const uint8_t *ptr = (const uint8_t *)eventbus_ring_vaddr + offset;
    uint32_t dispatched = 0;
    uint32_t dropped    = 0;

    for (uint32_t i = 0; i < count; i++) {
        const eb_batch_event_t *entry = (const eb_batch_event_t *)ptr;

        uint32_t kind = 0;
        uint32_t tlen = entry->topic_len < 4u ? entry->topic_len : 4u;
        for (uint32_t b = 0; b < tlen; b++)
            kind |= ((uint32_t)(uint8_t)entry->data[b]) << (b * 8u);

        const uint8_t *payload = (const uint8_t *)entry->data + entry->topic_len;
        uint32_t plen = entry->payload_len;

        if (eventbus_write(kind, (uint32_t)badge, payload, plen) == SEL4_ERR_OK)
            dispatched++;
        else
            dropped++;

        ptr += EB_BATCH_STRIDE(entry);
    }

    if (dispatched > 0)
        eventbus_notify_all();

    data_wr32(rep->data, 0, dispatched);
    data_wr32(rep->data, 4, dropped);
    return SEL4_ERR_OK;
}

/*
 * handle_publish — default opcode handler (any unregistered opcode is treated
 * as a publish request where the opcode IS the event kind).
 *
 * Request data layout:
 *   data[0..3]  = source_pd (uint32; 0 uses badge as source)
 *
 * This handler is registered last with opcode 0xFFFFFFFF as a sentinel.
 * In the dispatch path, unmatched opcodes fall through to SEL4_ERR_INVALID_OP.
 * Callers that want to publish a generic event use MSG_EVENTBUS_PUBLISH (below).
 */
#ifndef MSG_EVENTBUS_PUBLISH
#define MSG_EVENTBUS_PUBLISH 0x0006u
#endif

static uint32_t handle_publish(sel4_badge_t badge,
                                const sel4_msg_t *req,
                                sel4_msg_t *rep,
                                void *ctx)
{
    (void)badge; (void)ctx;

    /*
     * The event kind is encoded in req->opcode (which equals MSG_EVENTBUS_PUBLISH
     * for a generic publish; for a kind-tagged publish the caller sets opcode
     * directly to the event kind uint16).
     */
    uint32_t kind      = req->opcode;
    uint32_t source_pd = data_rd32(req->data, 0);
    if (source_pd == 0) source_pd = (uint32_t)badge;

    uint32_t rc = eventbus_write(kind, source_pd, NULL, 0);

    rep->length = 0;
    if (rc == SEL4_ERR_OK) {
        eventbus_notify_all();
        return SEL4_ERR_OK;
    }
    return SEL4_ERR_NO_MEM;
}

/* ── Nameserver self-registration ───────────────────────────────────────────── */

#ifndef MSG_EVENT_SYSTEM_READY
#define MSG_EVENT_SYSTEM_READY 0x0403u
#endif

static void sel4_call_stub(seL4_CPtr ep, const sel4_msg_t *req, sel4_msg_t *rep);

#ifdef AGENTOS_TEST_HOST
static void sel4_call_stub(seL4_CPtr ep, const sel4_msg_t *req, sel4_msg_t *rep)
{
    (void)ep; (void)req;
    rep->opcode = NS_OK;
    rep->length = 0;
}
#define sel4_call sel4_call_stub
#endif

static void register_with_nameserver(seL4_CPtr ns_ep)
{
    if (!ns_ep) return;

    sel4_msg_t req, rep;
    req.opcode = OP_NS_REGISTER;

    data_wr32(req.data,  0, 0u);    /* channel_id */
    data_wr32(req.data,  4, 1u);    /* pd_id = TRACE_PD_EVENT_BUS */
    data_wr32(req.data,  8, 0u);    /* cap_classes */
    data_wr32(req.data, 12, 1u);    /* version */

    const char *name = "event_bus";
    int i = 0;
    for (; name[i] && (16 + i) < 48; i++)
        req.data[16 + i] = (uint8_t)name[i];
    for (; (16 + i) < 48; i++)
        req.data[16 + i] = 0;

    req.length = 48;

    sel4_call(ns_ep, &req, &rep);
}

#ifdef AGENTOS_TEST_HOST
/* Un-define the macro alias so the production sel4_call isn't confused */
#undef sel4_call
#endif

/* ── Test-host entry points ─────────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST

/*
 * event_bus_test_init — reset all state and register handlers.
 *
 * Called by the test harness before each group of tests.  Requires
 * eventbus_ring_vaddr to be set by the test to a valid buffer.
 */
void event_bus_test_init(void)
{
    sub_count        = 0;
    event_seq        = 0;
    ring_initialized = false;

    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        subscribers[i].active   = false;
        subscribers[i].ntfn_cap = 0;
        subscribers[i].topic_mask = 0;
        subscribers[i].last_seq   = 0;
    }

    if (eventbus_ring_vaddr) eventbus_init_ring();

    sel4_server_init(&g_srv, 0 /* ep unused in tests */);
    sel4_server_register(&g_srv, MSG_EVENTBUS_INIT,       handle_init,          (void *)0);
    sel4_server_register(&g_srv, MSG_EVENTBUS_SUBSCRIBE,  handle_subscribe,     (void *)0);
    sel4_server_register(&g_srv, MSG_EVENTBUS_UNSUBSCRIBE,handle_unsubscribe,   (void *)0);
    sel4_server_register(&g_srv, MSG_EVENTBUS_STATUS,     handle_status,        (void *)0);
    sel4_server_register(&g_srv, OP_PUBLISH_BATCH,        handle_publish_batch, (void *)0);
    sel4_server_register(&g_srv, MSG_EVENTBUS_PUBLISH,    handle_publish,       (void *)0);
}

/*
 * event_bus_dispatch_one — exercise one IPC round-trip through the
 * sel4_server dispatch machinery without seL4.
 */
uint32_t event_bus_dispatch_one(sel4_badge_t badge,
                                 const sel4_msg_t *req,
                                 sel4_msg_t *rep)
{
    return sel4_server_dispatch(&g_srv, badge, req, rep);
}

/*
 * event_bus_get_sub_count — test helper to read subscriber count.
 */
uint32_t event_bus_get_sub_count(void) { return sub_count; }

/*
 * event_bus_get_event_seq — test helper to read event sequence counter.
 */
uint64_t event_bus_get_event_seq(void) { return event_seq; }

#else /* !AGENTOS_TEST_HOST — production build */

/*
 * event_bus_main — production entry point called by the root task boot
 * dispatcher.
 *
 * my_ep:  listen endpoint capability.
 * ns_ep:  nameserver endpoint (0 = nameserver not yet available).
 *
 * This function NEVER RETURNS.
 */
void event_bus_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    sub_count        = 0;
    event_seq        = 0;
    ring_initialized = false;

    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        subscribers[i].active   = false;
        subscribers[i].ntfn_cap = 0;
    }

    dbg_puts("[event_bus] starting\n");

    /* Initialise ring if the shared memory region is already mapped */
    if (eventbus_ring_vaddr)
        eventbus_init_ring();

    /* Self-register with nameserver */
    register_with_nameserver(ns_ep);

    dbg_puts("[event_bus] ready\n");

    sel4_server_init(&g_srv, my_ep);
    sel4_server_register(&g_srv, MSG_EVENTBUS_INIT,       handle_init,          (void *)0);
    sel4_server_register(&g_srv, MSG_EVENTBUS_SUBSCRIBE,  handle_subscribe,     (void *)0);
    sel4_server_register(&g_srv, MSG_EVENTBUS_UNSUBSCRIBE,handle_unsubscribe,   (void *)0);
    sel4_server_register(&g_srv, MSG_EVENTBUS_STATUS,     handle_status,        (void *)0);
    sel4_server_register(&g_srv, OP_PUBLISH_BATCH,        handle_publish_batch, (void *)0);
    sel4_server_register(&g_srv, MSG_EVENTBUS_PUBLISH,    handle_publish,       (void *)0);

    /* Enter the recv/dispatch/reply loop — never returns */
    sel4_server_run(&g_srv);
}

#endif /* AGENTOS_TEST_HOST */
