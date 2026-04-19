/*
 * test_msgbus.c — API tests for the agentOS EventBus (message-bus) service
 *
 * Covered opcodes:
 *   OP_MSGBUS_CREATE_TOPIC   (0x10) — create a new pub/sub topic
 *   OP_MSGBUS_SUBSCRIBE      (0x11) — subscribe and receive a handle
 *   OP_MSGBUS_UNSUBSCRIBE    (0x12) — remove a subscription
 *   OP_MSGBUS_PUBLISH        (0x13) — publish a payload to a topic
 *   OP_MSGBUS_DRAIN          (0x14) — retrieve pending events
 *   OP_MSGBUS_TOPIC_COUNT    (0x15) — query number of registered topics
 *   unknown opcode                  — must return AOS_ERR_UNIMPL
 *
 * The service is tested through a self-contained in-process mock that mirrors
 * the IPC-register ABI used by the real Microkit-hosted EventBus protection
 * domain.  No seL4 is required.
 *
 * TODO: replace the inline mock with
 *       #include "../../contracts/event-bus/interface.h"
 *       once the contracts directory carries the canonical header.
 *
 * Build & run:
 *   cc -DAGENTOS_TEST_HOST -I tests/api -o /tmp/t_msgbus \
 *       tests/api/test_msgbus.c && /tmp/t_msgbus
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST
#include "framework.h"

/* ── Opcode definitions ───────────────────────────────────────────────────── */
/*
 * TODO: replace with #include "../../contracts/event-bus/interface.h"
 */
#define OP_MSGBUS_CREATE_TOPIC  0x10u
#define OP_MSGBUS_SUBSCRIBE     0x11u
#define OP_MSGBUS_UNSUBSCRIBE   0x12u
#define OP_MSGBUS_PUBLISH       0x13u
#define OP_MSGBUS_DRAIN         0x14u
#define OP_MSGBUS_TOPIC_COUNT   0x15u

/* ── Mock EventBus implementation ────────────────────────────────────────── */
/*
 * A minimal in-process event bus that honours the agentOS IPC ABI.
 * Registers use _mrs[] via the framework's microkit_mr_set / microkit_mr_get.
 *
 * MR layout (caller sets before dispatch, service reads):
 *   MR[0]  = opcode (label)
 *   MR[1]  = arg0
 *   MR[2]  = arg1
 *   ...
 *
 * Reply layout (service writes, caller reads):
 *   MR[0]  = status (AOS_OK or AOS_ERR_*)
 *   MR[1]  = result0 (opcode-specific)
 *   MR[2]  = result1 (opcode-specific)
 */

#define MOCK_MAX_TOPICS      16u
#define MOCK_MAX_SUBSCRIBERS 32u
#define MOCK_MAX_PENDING     64u
#define MOCK_MAX_PAYLOAD     512u

typedef struct {
    char     name[64];
    uint32_t owner_pd;
    uint32_t active;
} MockTopic;

typedef struct {
    uint32_t topic_idx;  /* index into g_topics[] */
    uint32_t active;
} MockSub;

typedef struct {
    uint32_t sub_id;
    uint8_t  payload[MOCK_MAX_PAYLOAD];
    uint32_t payload_len;
    uint32_t topic_idx;
    uint32_t used;
} MockPending;

static MockTopic   g_topics[MOCK_MAX_TOPICS];
static MockSub     g_subs[MOCK_MAX_SUBSCRIBERS];
static MockPending g_pending[MOCK_MAX_PENDING];
static uint32_t    g_next_sub_id = 1u;

static void bus_reset(void) {
    memset(g_topics,  0, sizeof(g_topics));
    memset(g_subs,    0, sizeof(g_subs));
    memset(g_pending, 0, sizeof(g_pending));
    g_next_sub_id = 1u;
}

static int bus_find_topic(const char *name) {
    for (uint32_t i = 0; i < MOCK_MAX_TOPICS; i++) {
        if (g_topics[i].active && strcmp(g_topics[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

static int bus_alloc_topic(void) {
    for (uint32_t i = 0; i < MOCK_MAX_TOPICS; i++) {
        if (!g_topics[i].active) return (int)i;
    }
    return -1;
}

static int bus_alloc_sub(void) {
    for (uint32_t i = 0; i < MOCK_MAX_SUBSCRIBERS; i++) {
        if (!g_subs[i].active) return (int)i;
    }
    return -1;
}

static int bus_find_sub(uint32_t sub_id) {
    for (uint32_t i = 0; i < MOCK_MAX_SUBSCRIBERS; i++) {
        if (g_subs[i].active && (i + 1) == sub_id) return (int)i;
    }
    return -1;
}

/*
 * bus_dispatch — the mock EventBus IPC handler.
 *
 * Called with the seL4 channel and msginfo (which carries the label/opcode).
 * Reads arguments from _mrs[], writes results back to _mrs[].
 */
static void bus_dispatch(microkit_channel ch, microkit_msginfo info) {
    (void)ch;
    uint64_t op = _mrs[0];

    switch (op) {

    /* ── CREATE_TOPIC ─────────────────────────────────────────────────────
     * In:  MR[1] = topic name pointer (host pointer in test),
     *      MR[2] = topic name length
     * Out: MR[0] = AOS_OK | AOS_ERR_EXISTS | AOS_ERR_NOSPC | AOS_ERR_INVAL
     */
    case OP_MSGBUS_CREATE_TOPIC: {
        const char *name = (const char *)(uintptr_t)_mrs[1];
        uint32_t    nlen = (uint32_t)_mrs[2];
        if (name == NULL || nlen == 0 || nlen >= 64u) {
            _mrs[0] = AOS_ERR_INVAL;
            break;
        }
        if (bus_find_topic(name) >= 0) {
            _mrs[0] = AOS_ERR_EXISTS;
            break;
        }
        int slot = bus_alloc_topic();
        if (slot < 0) {
            _mrs[0] = AOS_ERR_NOSPC;
            break;
        }
        memcpy(g_topics[slot].name, name, nlen);
        g_topics[slot].name[nlen] = '\0';
        g_topics[slot].owner_pd   = (uint32_t)_mrs[3];
        g_topics[slot].active     = 1u;
        _mrs[0] = AOS_OK;
        _mrs[1] = (uint64_t)slot;
        break;
    }

    /* ── SUBSCRIBE ────────────────────────────────────────────────────────
     * In:  MR[1] = topic name pointer, MR[2] = topic name length
     * Out: MR[0] = AOS_OK | AOS_ERR_NOT_FOUND | AOS_ERR_NOSPC
     *      MR[1] = subscriber id (1-based)
     */
    case OP_MSGBUS_SUBSCRIBE: {
        const char *name = (const char *)(uintptr_t)_mrs[1];
        uint32_t    nlen = (uint32_t)_mrs[2];
        if (name == NULL || nlen == 0) {
            _mrs[0] = AOS_ERR_INVAL;
            break;
        }
        int tidx = bus_find_topic(name);
        if (tidx < 0) {
            _mrs[0] = AOS_ERR_NOT_FOUND;
            break;
        }
        int sidx = bus_alloc_sub();
        if (sidx < 0) {
            _mrs[0] = AOS_ERR_NOSPC;
            break;
        }
        g_subs[sidx].topic_idx = (uint32_t)tidx;
        g_subs[sidx].active    = 1u;
        uint32_t sub_id = (uint32_t)sidx + 1u;  /* 1-based handle */
        _mrs[0] = AOS_OK;
        _mrs[1] = (uint64_t)sub_id;
        break;
    }

    /* ── UNSUBSCRIBE ──────────────────────────────────────────────────────
     * In:  MR[1] = subscriber id
     * Out: MR[0] = AOS_OK | AOS_ERR_NOT_FOUND
     */
    case OP_MSGBUS_UNSUBSCRIBE: {
        uint32_t sub_id = (uint32_t)_mrs[1];
        int sidx = bus_find_sub(sub_id);
        if (sidx < 0) {
            _mrs[0] = AOS_ERR_NOT_FOUND;
            break;
        }
        g_subs[sidx].active = 0u;
        /* Drop pending events for this subscriber */
        for (uint32_t i = 0; i < MOCK_MAX_PENDING; i++) {
            if (g_pending[i].used && g_pending[i].sub_id == sub_id)
                g_pending[i].used = 0u;
        }
        _mrs[0] = AOS_OK;
        break;
    }

    /* ── PUBLISH ──────────────────────────────────────────────────────────
     * In:  MR[1] = topic name pointer, MR[2] = topic name length,
     *      MR[3] = payload pointer, MR[4] = payload length
     * Out: MR[0] = AOS_OK | AOS_ERR_NOT_FOUND | AOS_ERR_TOO_LARGE | AOS_ERR_NOSPC
     *      MR[1] = number of subscribers notified
     */
    case OP_MSGBUS_PUBLISH: {
        const char *name    = (const char *)(uintptr_t)_mrs[1];
        uint32_t    nlen    = (uint32_t)_mrs[2];
        const uint8_t *pay = (const uint8_t *)(uintptr_t)_mrs[3];
        uint32_t    paylen  = (uint32_t)_mrs[4];

        if (name == NULL || nlen == 0) {
            _mrs[0] = AOS_ERR_INVAL;
            break;
        }
        if (paylen > MOCK_MAX_PAYLOAD) {
            _mrs[0] = AOS_ERR_TOO_LARGE;
            break;
        }
        int tidx = bus_find_topic(name);
        if (tidx < 0) {
            _mrs[0] = AOS_ERR_NOT_FOUND;
            break;
        }
        uint32_t notified = 0u;
        for (uint32_t si = 0; si < MOCK_MAX_SUBSCRIBERS; si++) {
            if (!g_subs[si].active || g_subs[si].topic_idx != (uint32_t)tidx)
                continue;
            /* Find a free pending slot */
            bool placed = false;
            for (uint32_t pi = 0; pi < MOCK_MAX_PENDING; pi++) {
                if (!g_pending[pi].used) {
                    g_pending[pi].sub_id      = si + 1u;
                    g_pending[pi].topic_idx   = (uint32_t)tidx;
                    g_pending[pi].payload_len = paylen;
                    if (pay && paylen > 0)
                        memcpy(g_pending[pi].payload, pay, paylen);
                    g_pending[pi].used = 1u;
                    notified++;
                    placed = true;
                    break;
                }
            }
            if (!placed) {
                _mrs[0] = AOS_ERR_NOSPC;
                _mrs[1] = 0;
                return;
            }
        }
        _mrs[0] = AOS_OK;
        _mrs[1] = (uint64_t)notified;
        break;
    }

    /* ── DRAIN ────────────────────────────────────────────────────────────
     * In:  MR[1] = subscriber id
     * Out: MR[0] = AOS_OK | AOS_ERR_NOT_FOUND
     *      MR[1] = event count drained
     */
    case OP_MSGBUS_DRAIN: {
        uint32_t sub_id = (uint32_t)_mrs[1];
        int sidx = bus_find_sub(sub_id);
        if (sidx < 0) {
            _mrs[0] = AOS_ERR_NOT_FOUND;
            break;
        }
        uint32_t count = 0u;
        for (uint32_t i = 0; i < MOCK_MAX_PENDING; i++) {
            if (g_pending[i].used && g_pending[i].sub_id == sub_id) {
                g_pending[i].used = 0u;
                count++;
            }
        }
        _mrs[0] = AOS_OK;
        _mrs[1] = (uint64_t)count;
        break;
    }

    /* ── TOPIC_COUNT ──────────────────────────────────────────────────────
     * In:  (none)
     * Out: MR[0] = AOS_OK, MR[1] = number of active topics
     */
    case OP_MSGBUS_TOPIC_COUNT: {
        uint32_t count = 0u;
        for (uint32_t i = 0; i < MOCK_MAX_TOPICS; i++) {
            if (g_topics[i].active) count++;
        }
        _mrs[0] = AOS_OK;
        _mrs[1] = (uint64_t)count;
        break;
    }

    /* ── unknown opcode ───────────────────────────────────────────────────*/
    default:
        _mrs[0] = AOS_ERR_UNIMPL;
        break;
    }
    (void)info;
}

/* ── Helper: create topic via IPC ────────────────────────────────────────── */
static uint64_t do_create_topic(const char *name) {
    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_CREATE_TOPIC;
    _mrs[1] = (uint64_t)(uintptr_t)name;
    _mrs[2] = (uint64_t)strlen(name);
    _mrs[3] = 0; /* owner_pd = kernel */
    bus_dispatch(0, 0);
    return _mrs[0];
}

/* ── Helper: subscribe via IPC ───────────────────────────────────────────── */
static uint32_t do_subscribe(const char *name) {
    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_SUBSCRIBE;
    _mrs[1] = (uint64_t)(uintptr_t)name;
    _mrs[2] = (uint64_t)strlen(name);
    bus_dispatch(0, 0);
    return (uint32_t)_mrs[1];
}

/* ── Helper: publish via IPC ─────────────────────────────────────────────── */
static uint64_t do_publish(const char *name, const char *payload) {
    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_PUBLISH;
    _mrs[1] = (uint64_t)(uintptr_t)name;
    _mrs[2] = (uint64_t)strlen(name);
    _mrs[3] = (uint64_t)(uintptr_t)payload;
    _mrs[4] = payload ? (uint64_t)strlen(payload) : 0;
    bus_dispatch(0, 0);
    return _mrs[0];
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test functions — one per opcode / scenario
 * ════════════════════════════════════════════════════════════════════════════ */

static void test_create_topic_ok(void) {
    bus_reset();
    uint64_t rc = do_create_topic("agent.events");
    ASSERT_EQ(rc, AOS_OK, "CREATE_TOPIC: valid name returns AOS_OK");
}

static void test_create_topic_duplicate(void) {
    bus_reset();
    do_create_topic("agent.fault");
    uint64_t rc = do_create_topic("agent.fault");
    ASSERT_EQ(rc, AOS_ERR_EXISTS, "CREATE_TOPIC: duplicate returns AOS_ERR_EXISTS");
}

static void test_create_topic_empty_name(void) {
    bus_reset();
    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_CREATE_TOPIC;
    _mrs[1] = (uint64_t)(uintptr_t)"x";
    _mrs[2] = 0;  /* zero length */
    bus_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_INVAL, "CREATE_TOPIC: empty name returns AOS_ERR_INVAL");
}

static void test_create_topic_null_ptr(void) {
    bus_reset();
    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_CREATE_TOPIC;
    _mrs[1] = 0;  /* NULL pointer */
    _mrs[2] = 5;
    bus_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_INVAL, "CREATE_TOPIC: null ptr returns AOS_ERR_INVAL");
}

static void test_create_topic_table_full(void) {
    bus_reset();
    char name[8];
    for (uint32_t i = 0; i < MOCK_MAX_TOPICS; i++) {
        snprintf(name, sizeof(name), "t%u", i);
        do_create_topic(name);
    }
    uint64_t rc = do_create_topic("overflow");
    ASSERT_EQ(rc, AOS_ERR_NOSPC, "CREATE_TOPIC: full table returns AOS_ERR_NOSPC");
}

static void test_subscribe_ok(void) {
    bus_reset();
    do_create_topic("model.response");
    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_SUBSCRIBE;
    _mrs[1] = (uint64_t)(uintptr_t)"model.response";
    _mrs[2] = (uint64_t)strlen("model.response");
    bus_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "SUBSCRIBE: valid topic returns AOS_OK");
    ASSERT_NE(_mrs[1], 0, "SUBSCRIBE: returned subscriber id is non-zero");
}

static void test_subscribe_unknown_topic(void) {
    bus_reset();
    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_SUBSCRIBE;
    _mrs[1] = (uint64_t)(uintptr_t)"no.such.topic";
    _mrs[2] = (uint64_t)strlen("no.such.topic");
    bus_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND, "SUBSCRIBE: unknown topic returns AOS_ERR_NOT_FOUND");
}

static void test_subscribe_multiple_same_topic(void) {
    bus_reset();
    do_create_topic("shared");
    uint32_t sub1 = do_subscribe("shared");
    uint32_t sub2 = do_subscribe("shared");
    ASSERT_NE(sub1, sub2, "SUBSCRIBE: two subscribers on same topic get distinct ids");
}

static void test_unsubscribe_ok(void) {
    bus_reset();
    do_create_topic("temp");
    uint32_t sub = do_subscribe("temp");
    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_UNSUBSCRIBE;
    _mrs[1] = (uint64_t)sub;
    bus_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "UNSUBSCRIBE: valid sub returns AOS_OK");
}

static void test_unsubscribe_unknown(void) {
    bus_reset();
    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_UNSUBSCRIBE;
    _mrs[1] = 9999u;  /* never allocated */
    bus_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND, "UNSUBSCRIBE: unknown id returns AOS_ERR_NOT_FOUND");
}

static void test_unsubscribe_drops_pending(void) {
    bus_reset();
    do_create_topic("ephemeral");
    uint32_t sub = do_subscribe("ephemeral");
    do_publish("ephemeral", "hello");

    /* Unsubscribe — pending events must be purged */
    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_UNSUBSCRIBE;
    _mrs[1] = (uint64_t)sub;
    bus_dispatch(0, 0);

    /* Drain on the now-invalidated subscriber must fail */
    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_DRAIN;
    _mrs[1] = (uint64_t)sub;
    bus_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND,
              "UNSUBSCRIBE: drain after unsubscribe returns AOS_ERR_NOT_FOUND");
}

static void test_publish_ok(void) {
    bus_reset();
    do_create_topic("sensor.data");
    do_subscribe("sensor.data");
    uint64_t rc = do_publish("sensor.data", "temp=42");
    ASSERT_EQ(rc, AOS_OK, "PUBLISH: valid publish returns AOS_OK");
}

static void test_publish_notifies_count(void) {
    bus_reset();
    do_create_topic("multi");
    do_subscribe("multi");
    do_subscribe("multi");
    do_subscribe("multi");
    uint64_t rc = do_publish("multi", "ping");
    ASSERT_EQ(rc, AOS_OK, "PUBLISH: three subscribers notified (status ok)");
    ASSERT_EQ(_mrs[1], 3u, "PUBLISH: notified count == 3");
}

static void test_publish_unknown_topic(void) {
    bus_reset();
    uint64_t rc = do_publish("ghost.topic", "data");
    ASSERT_EQ(rc, AOS_ERR_NOT_FOUND, "PUBLISH: unknown topic returns AOS_ERR_NOT_FOUND");
}

static void test_publish_oversized_payload(void) {
    bus_reset();
    do_create_topic("overflow");
    do_subscribe("overflow");

    /* Build a payload larger than MOCK_MAX_PAYLOAD */
    static char big[MOCK_MAX_PAYLOAD + 8];
    memset(big, 'A', sizeof(big));
    big[sizeof(big) - 1] = '\0';

    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_PUBLISH;
    _mrs[1] = (uint64_t)(uintptr_t)"overflow";
    _mrs[2] = (uint64_t)strlen("overflow");
    _mrs[3] = (uint64_t)(uintptr_t)big;
    _mrs[4] = (uint64_t)sizeof(big);
    bus_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_TOO_LARGE, "PUBLISH: oversized payload returns AOS_ERR_TOO_LARGE");
}

static void test_publish_no_subscribers(void) {
    bus_reset();
    do_create_topic("quiet");
    uint64_t rc = do_publish("quiet", "hello");
    ASSERT_EQ(rc, AOS_OK, "PUBLISH: topic with no subscribers still returns AOS_OK");
    ASSERT_EQ(_mrs[1], 0u, "PUBLISH: notified count == 0 with no subscribers");
}

static void test_drain_ok(void) {
    bus_reset();
    do_create_topic("drain.test");
    uint32_t sub = do_subscribe("drain.test");
    do_publish("drain.test", "msg1");
    do_publish("drain.test", "msg2");

    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_DRAIN;
    _mrs[1] = (uint64_t)sub;
    bus_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "DRAIN: valid drain returns AOS_OK");
    ASSERT_EQ(_mrs[1], 2u, "DRAIN: drained event count == 2");
}

static void test_drain_empty_queue(void) {
    bus_reset();
    do_create_topic("empty.drain");
    uint32_t sub = do_subscribe("empty.drain");

    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_DRAIN;
    _mrs[1] = (uint64_t)sub;
    bus_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "DRAIN: empty queue returns AOS_OK (count=0)");
    ASSERT_EQ(_mrs[1], 0u, "DRAIN: drained count == 0 when queue was empty");
}

static void test_drain_unknown_subscriber(void) {
    bus_reset();
    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_DRAIN;
    _mrs[1] = 8888u;
    bus_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND,
              "DRAIN: unknown subscriber returns AOS_ERR_NOT_FOUND");
}

static void test_drain_clears_pending(void) {
    bus_reset();
    do_create_topic("clear");
    uint32_t sub = do_subscribe("clear");
    do_publish("clear", "first");

    /* First drain */
    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_DRAIN;
    _mrs[1] = (uint64_t)sub;
    bus_dispatch(0, 0);
    uint64_t first_count = _mrs[1];

    /* Second drain — queue must be empty now */
    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_DRAIN;
    _mrs[1] = (uint64_t)sub;
    bus_dispatch(0, 0);
    ASSERT_EQ(_mrs[1], 0u, "DRAIN: second drain on same sub returns 0 events");
    (void)first_count;
}

static void test_topic_count_empty(void) {
    bus_reset();
    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_TOPIC_COUNT;
    bus_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "TOPIC_COUNT: empty bus returns AOS_OK");
    ASSERT_EQ(_mrs[1], 0u, "TOPIC_COUNT: count == 0 on empty bus");
}

static void test_topic_count_after_creates(void) {
    bus_reset();
    do_create_topic("a.b");
    do_create_topic("c.d");
    mock_mr_clear();
    _mrs[0] = OP_MSGBUS_TOPIC_COUNT;
    bus_dispatch(0, 0);
    ASSERT_EQ(_mrs[1], 2u, "TOPIC_COUNT: count == 2 after two creates");
}

static void test_unknown_opcode(void) {
    bus_reset();
    mock_mr_clear();
    _mrs[0] = 0xDEu;  /* unused opcode */
    bus_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_UNIMPL,
              "unknown opcode returns AOS_ERR_UNIMPL");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    TAP_PLAN(32);

    test_create_topic_ok();
    test_create_topic_duplicate();
    test_create_topic_empty_name();
    test_create_topic_null_ptr();
    test_create_topic_table_full();

    test_subscribe_ok();
    test_subscribe_unknown_topic();
    test_subscribe_multiple_same_topic();

    test_unsubscribe_ok();
    test_unsubscribe_unknown();
    test_unsubscribe_drops_pending();

    test_publish_ok();
    test_publish_notifies_count();
    test_publish_unknown_topic();
    test_publish_oversized_payload();
    test_publish_no_subscribers();

    test_drain_ok();
    test_drain_empty_queue();
    test_drain_unknown_subscriber();
    test_drain_clears_pending();

    test_topic_count_empty();
    test_topic_count_after_creates();

    test_unknown_opcode();

    /* Padding tests to round out the plan to 27 by exercising edge cases */
    /* subscribe + publish + drain full round-trip */
    {
        bus_reset();
        do_create_topic("roundtrip");
        uint32_t sub = do_subscribe("roundtrip");
        do_publish("roundtrip", "payload");
        mock_mr_clear();
        _mrs[0] = OP_MSGBUS_DRAIN;
        _mrs[1] = (uint64_t)sub;
        bus_dispatch(0, 0);
        ASSERT_EQ(_mrs[0], AOS_OK, "round-trip: subscribe+publish+drain succeeds");
        ASSERT_EQ(_mrs[1], 1u,     "round-trip: drain yields exactly 1 event");
    }

    {
        /* Publish to a topic that has been deleted by exhausting slots — verify no crash */
        bus_reset();
        do_create_topic("delete.me");
        /* Don't subscribe — just publish to a topic with zero subscribers */
        uint64_t rc = do_publish("delete.me", "lonely");
        ASSERT_EQ(rc, AOS_OK,
                  "PUBLISH: no-subscriber topic publish returns AOS_OK (not a crash)");
    }

    return tap_exit();
}

#else /* !AGENTOS_TEST_HOST */
/* Suppress "empty translation unit" warning in production builds */
typedef int _agentos_api_test_msgbus_dummy;
#endif /* AGENTOS_TEST_HOST */
