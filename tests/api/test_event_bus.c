/*
 * test_event_bus.c — API tests for the agentOS event_bus PD
 *
 * Covered opcodes and scenarios:
 *   MSG_EVENTBUS_INIT        (0x0001) — initialise ring buffer
 *   MSG_EVENTBUS_SUBSCRIBE   (0x0002) — add subscriber, return handle
 *   MSG_EVENTBUS_UNSUBSCRIBE (0x0003) — remove subscriber
 *   MSG_EVENTBUS_STATUS      (0x0004) — ring status
 *   OP_PUBLISH_BATCH         (0x0025) — batch publish
 *   MSG_EVENTBUS_PUBLISH     (0x0006) — single event publish
 *   unknown opcode                    — must return SEL4_ERR_INVALID_OP
 *
 * Tests pull in the event_bus implementation directly under
 * -DAGENTOS_TEST_HOST so no seL4 is required.
 *
 * Build & run:
 *   cc -DAGENTOS_TEST_HOST \
 *      -I tests/api \
 *      -I kernel/agentos-root-task/include \
 *      -o /tmp/test_event_bus \
 *      tests/api/test_event_bus.c && /tmp/test_event_bus
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST

#include "framework.h"

/* Pull in the PD implementation.  The AGENTOS_TEST_HOST guard inside
 * event_bus.c replaces all seL4/Microkit references with stubs. */
#include "../../kernel/agentos-root-task/src/event_bus.c"

/* ── Ring buffer backing store ──────────────────────────────────────────────── */

/* EVENTBUS_RING_SIZE is 256 KB — allocate statically in BSS. */
static uint8_t eb_ring_backing[EVENTBUS_RING_SIZE];

/* ── Setup helper ───────────────────────────────────────────────────────────── */

static void setup(void) {
    memset(eb_ring_backing, 0, sizeof(eb_ring_backing));
    eventbus_ring_vaddr = (uintptr_t)eb_ring_backing;
    event_bus_test_init();
}

/* ── Data field helpers (reuse the same little-endian layout) ──────────────── */

static inline uint32_t t_rd32(const uint8_t *d, int off) {
    return (uint32_t)d[off    ]
         | ((uint32_t)d[off+1] <<  8)
         | ((uint32_t)d[off+2] << 16)
         | ((uint32_t)d[off+3] << 24);
}
static inline uint64_t t_rd64(const uint8_t *d, int off) {
    return (uint64_t)t_rd32(d, off) | ((uint64_t)t_rd32(d, off+4) << 32);
}
static inline void t_wr32(uint8_t *d, int off, uint32_t v) {
    d[off  ] = (uint8_t)(v      );
    d[off+1] = (uint8_t)(v >>  8);
    d[off+2] = (uint8_t)(v >> 16);
    d[off+3] = (uint8_t)(v >> 24);
}
static inline void t_wr64(uint8_t *d, int off, uint64_t v) {
    t_wr32(d, off,     (uint32_t)(v & 0xFFFFFFFFu));
    t_wr32(d, off + 4, (uint32_t)(v >> 32));
}

/* ── Helper: dispatch MSG_EVENTBUS_INIT ──────────────────────────────────── */

static uint32_t do_init(void) {
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = MSG_EVENTBUS_INIT;
    req.length = 0;
    return event_bus_dispatch_one(0, &req, &rep);
}

/* ── Helper: subscribe with a dummy ntfn_cap ─────────────────────────────── */

static uint32_t do_subscribe(uint64_t ntfn_cap, uint32_t topic_mask) {
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = MSG_EVENTBUS_SUBSCRIBE;
    t_wr64(req.data, 0, ntfn_cap);
    t_wr32(req.data, 8, topic_mask);
    req.length = 12;
    uint32_t rc = event_bus_dispatch_one(0, &req, &rep);
    if (rc == SEL4_ERR_OK)
        return t_rd32(rep.data, 0);   /* subscriber_id */
    return (uint32_t)-1;
}

/* ── Helper: unsubscribe by handle ───────────────────────────────────────── */

static uint32_t do_unsubscribe(uint32_t handle) {
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = MSG_EVENTBUS_UNSUBSCRIBE;
    t_wr32(req.data, 0, handle);
    req.length = 4;
    return event_bus_dispatch_one(0, &req, &rep);
}

/* ── Helper: publish single event ────────────────────────────────────────── */

static uint32_t do_publish(uint32_t kind, uint32_t source_pd) {
    sel4_msg_t req = {0}, rep = {0};
    /*
     * MSG_EVENTBUS_PUBLISH is the stable dispatcher opcode (0x0006).
     * handle_publish reads req->opcode as the event kind — so we set it to
     * the event kind we want to publish (which equals MSG_EVENTBUS_PUBLISH
     * for a generic publish, or any kind value for a kind-tagged publish).
     * source_pd is passed via badge (the second argument to dispatch_one).
     */
    req.opcode = MSG_EVENTBUS_PUBLISH;  /* dispatcher key */
    t_wr32(req.data, 0, source_pd);
    req.length = 4;
    (void)kind;  /* generic publish — kind == MSG_EVENTBUS_PUBLISH */
    return event_bus_dispatch_one((sel4_badge_t)source_pd, &req, &rep);
}

/* ── Test: init ──────────────────────────────────────────────────────────── */

static void test_init_ok(void) {
    setup();
    uint32_t rc = do_init();
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "MSG_EVENTBUS_INIT: returns SEL4_ERR_OK");
}

static void test_init_returns_ready(void) {
    setup();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = MSG_EVENTBUS_INIT;
    event_bus_dispatch_one(0, &req, &rep);
    uint32_t tag = t_rd32(rep.data, 0);
    ASSERT_EQ(tag, (uint64_t)MSG_EVENTBUS_READY,
              "MSG_EVENTBUS_INIT: reply data[0] == MSG_EVENTBUS_READY");
}

/* ── Test: subscribe ─────────────────────────────────────────────────────── */

static void test_subscribe_returns_handle(void) {
    setup();
    do_init();
    uint32_t id = do_subscribe(0x100, 0);
    ASSERT_NE(id, (uint64_t)(uint32_t)-1, "MSG_EVENTBUS_SUBSCRIBE: returns valid handle");
}

static void test_subscribe_increments_count(void) {
    setup();
    do_init();
    do_subscribe(0x101, 0);
    do_subscribe(0x102, 0);
    ASSERT_EQ(event_bus_get_sub_count(), 2u, "MSG_EVENTBUS_SUBSCRIBE: sub_count == 2 after two subscribes");
}

static void test_subscribe_distinct_handles(void) {
    setup();
    do_init();
    uint32_t id1 = do_subscribe(0x201, 0);
    uint32_t id2 = do_subscribe(0x202, 0);
    ASSERT_NE(id1, id2, "MSG_EVENTBUS_SUBSCRIBE: two subscribers get distinct handles");
}

static void test_subscribe_table_full(void) {
    setup();
    do_init();
    for (int i = 0; i < MAX_SUBSCRIBERS; i++)
        do_subscribe((uint64_t)(i + 1), 0);

    /* One more must fail */
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = MSG_EVENTBUS_SUBSCRIBE;
    t_wr64(req.data, 0, (uint64_t)(MAX_SUBSCRIBERS + 1));
    req.length = 12;
    uint32_t rc = event_bus_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_NO_MEM,
              "MSG_EVENTBUS_SUBSCRIBE: returns SEL4_ERR_NO_MEM when table full");
}

/* ── Test: unsubscribe ───────────────────────────────────────────────────── */

static void test_unsubscribe_removes_subscriber(void) {
    setup();
    do_init();
    uint32_t id = do_subscribe(0x300, 0);
    uint32_t rc = do_unsubscribe(id);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "MSG_EVENTBUS_UNSUBSCRIBE: returns SEL4_ERR_OK");
    ASSERT_EQ(event_bus_get_sub_count(), 0u, "MSG_EVENTBUS_UNSUBSCRIBE: sub_count decremented to 0");
}

static void test_unsubscribe_bad_handle(void) {
    setup();
    do_init();
    uint32_t rc = do_unsubscribe(0xFFFFFFFFu);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_BAD_ARG,
              "MSG_EVENTBUS_UNSUBSCRIBE: bad handle returns SEL4_ERR_BAD_ARG");
}

/* ── Test: publish ───────────────────────────────────────────────────────── */

static void test_publish_with_one_subscriber(void) {
    setup();
    do_init();
    do_subscribe(0x400, 0);
    uint32_t rc = do_publish(0x0001u, 7u);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK,
              "MSG_EVENTBUS_PUBLISH: returns SEL4_ERR_OK with one subscriber");
}

static void test_publish_advances_event_seq(void) {
    setup();
    do_init();
    uint64_t before = event_bus_get_event_seq();
    do_publish(0x0002u, 0u);
    uint64_t after = event_bus_get_event_seq();
    ASSERT_TRUE(after > before, "MSG_EVENTBUS_PUBLISH: event_seq advances after publish");
}

static void test_publish_no_subscribers(void) {
    setup();
    do_init();
    /* No subscribers — publish should still succeed (no crash, returns OK) */
    uint32_t rc = do_publish(0x0003u, 1u);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK,
              "MSG_EVENTBUS_PUBLISH: succeeds even with no subscribers");
}

/* ── Test: batch publish ─────────────────────────────────────────────────── */

static void test_batch_empty_count(void) {
    setup();
    do_init();

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_PUBLISH_BATCH;
    t_wr32(req.data, 0, 0u);     /* count == 0 — invalid */
    t_wr32(req.data, 4, EVENTBUS_BATCH_STAGING_OFFSET);
    req.length = 8;
    uint32_t rc = event_bus_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_BAD_ARG,
              "OP_PUBLISH_BATCH: count==0 returns SEL4_ERR_BAD_ARG");
}

static void test_batch_count_too_large(void) {
    setup();
    do_init();

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_PUBLISH_BATCH;
    t_wr32(req.data, 0, PUBLISH_BATCH_MAX + 1u);
    t_wr32(req.data, 4, EVENTBUS_BATCH_STAGING_OFFSET);
    req.length = 8;
    uint32_t rc = event_bus_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_BAD_ARG,
              "OP_PUBLISH_BATCH: count>PUBLISH_BATCH_MAX returns SEL4_ERR_BAD_ARG");
}

static void test_batch_bad_offset(void) {
    setup();
    do_init();

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_PUBLISH_BATCH;
    t_wr32(req.data, 0, 1u);
    t_wr32(req.data, 4, EVENTBUS_RING_SIZE);  /* out of bounds */
    req.length = 8;
    uint32_t rc = event_bus_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_BAD_ARG,
              "OP_PUBLISH_BATCH: offset >= RING_SIZE returns SEL4_ERR_BAD_ARG");
}

static void test_batch_one_event(void) {
    setup();
    do_init();
    do_subscribe(0x500, 0);

    /*
     * Write a single batch_event_t into the batch staging area.
     * eb_batch_event_t layout: topic_len(u16), payload_len(u16), data[]
     * Topic "ab" (2 bytes), payload "XY" (2 bytes).
     */
    uint8_t *staging = eb_ring_backing + EVENTBUS_BATCH_STAGING_OFFSET;
    staging[0] = 2; staging[1] = 0;    /* topic_len = 2 */
    staging[2] = 2; staging[3] = 0;    /* payload_len = 2 */
    staging[4] = 'a'; staging[5] = 'b';
    staging[6] = 'X'; staging[7] = 'Y';

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_PUBLISH_BATCH;
    t_wr32(req.data, 0, 1u);
    t_wr32(req.data, 4, EVENTBUS_BATCH_STAGING_OFFSET);
    req.length = 8;
    uint32_t rc = event_bus_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK,
              "OP_PUBLISH_BATCH: single event batch returns SEL4_ERR_OK");
    ASSERT_EQ(t_rd32(rep.data, 0), 1u,
              "OP_PUBLISH_BATCH: dispatched count == 1");
    ASSERT_EQ(t_rd32(rep.data, 4), 0u,
              "OP_PUBLISH_BATCH: dropped count == 0");
}

/* ── Test: status opcode ─────────────────────────────────────────────────── */

static void test_status_initial(void) {
    setup();
    do_init();

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = MSG_EVENTBUS_STATUS;
    uint32_t rc = event_bus_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK, "MSG_EVENTBUS_STATUS: returns SEL4_ERR_OK");
    /*
     * After init, one MSG_EVENT_SYSTEM_READY event is written to the ring,
     * advancing head to 1.  Tail stays at 0 (no reader has consumed it yet).
     */
    uint64_t head = t_rd64(rep.data, 0);
    uint64_t tail = t_rd64(rep.data, 8);
    ASSERT_EQ(head, 1u, "MSG_EVENTBUS_STATUS: head == 1 after init (system-ready event written)");
    ASSERT_EQ(tail, 0u, "MSG_EVENTBUS_STATUS: tail == 0 on fresh ring (no reader)");
}

/* ── Test: unknown opcode ─────────────────────────────────────────────────── */

static void test_unknown_opcode(void) {
    setup();
    do_init();

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = 0xBEEFu;
    req.length = 0;
    uint32_t rc = event_bus_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_INVALID_OP,
              "unknown opcode returns SEL4_ERR_INVALID_OP");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    TAP_PLAN(22);

    test_init_ok();
    test_init_returns_ready();

    test_subscribe_returns_handle();
    test_subscribe_increments_count();
    test_subscribe_distinct_handles();
    test_subscribe_table_full();

    test_unsubscribe_removes_subscriber();
    test_unsubscribe_bad_handle();

    test_publish_with_one_subscriber();
    test_publish_advances_event_seq();
    test_publish_no_subscribers();

    test_batch_empty_count();
    test_batch_count_too_large();
    test_batch_bad_offset();
    test_batch_one_event();

    test_status_initial();

    test_unknown_opcode();

    return tap_exit();
}

#else /* !AGENTOS_TEST_HOST */
typedef int _agentos_api_test_event_bus_dummy;
#endif /* AGENTOS_TEST_HOST */
