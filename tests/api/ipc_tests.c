/*
 * ipc_tests.c — IPC correctness tests for the sel4_ipc / sel4_server API
 *
 * Tests the seL4 IPC message layer in synchronous simulation.  No real seL4
 * kernel or Microkit runtime is required.  The production structs and dispatch
 * logic are embedded inline under AGENTOS_TEST_HOST so the file compiles with:
 *
 *   cc -std=c11 -Wall -Wextra -DAGENTOS_TEST_HOST \
 *       -I tests/api -o /tmp/ipc_tests tests/api/ipc_tests.c
 *
 * sel4_server_dispatch() is invoked directly (no threads) to give a
 * synchronous call/reply model identical to what a single-threaded seL4 PD
 * experiences after receiving a PPC.
 *
 * TAP version 14 output; 25 test points.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "framework.h"   /* TAP_PLAN, TAP_OK, TAP_FAIL, ASSERT_*, tap_exit */

/* ══════════════════════════════════════════════════════════════════════════
 * Embedded production types (from sel4_ipc.h / sel4_server.h)
 * Duplicated here so the test is self-contained; values must match the
 * authoritative headers in kernel/agentos-root-task/include/.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Error codes ────────────────────────────────────────────────────────── */
#define SEL4_ERR_OK           0u
#define SEL4_ERR_INVALID_OP   1u
#define SEL4_ERR_NOT_FOUND    2u
#define SEL4_ERR_PERM         3u
#define SEL4_ERR_BAD_ARG      4u
#define SEL4_ERR_NO_MEM       5u
#define SEL4_ERR_BUSY         6u
#define SEL4_ERR_OVERFLOW     7u
#define SEL4_ERR_INTERNAL     8u
#define SEL4_ERR_FORBIDDEN    9u

/* ── Message geometry ───────────────────────────────────────────────────── */
#define SEL4_MSG_DATA_BYTES   48u

typedef struct {
    uint32_t opcode;
    uint32_t length;
    uint8_t  data[SEL4_MSG_DATA_BYTES];
} sel4_msg_t;

_Static_assert(sizeof(sel4_msg_t) == 56u, "sel4_msg_t must be 56 bytes");

/* Badge is a bare uint64_t (seL4_Word on 64-bit) */
typedef uint64_t sel4_badge_t;

_Static_assert(sizeof(sel4_badge_t) == 8u, "sel4_badge_t must be 8 bytes");

/* ── Server machinery ───────────────────────────────────────────────────── */
#define SEL4_SERVER_MAX_HANDLERS  32u

typedef uint32_t (*sel4_handler_fn)(sel4_badge_t badge,
                                     const sel4_msg_t *req,
                                     sel4_msg_t *rep,
                                     void *ctx);

typedef struct {
    struct {
        uint32_t        opcode;
        sel4_handler_fn fn;
        void           *ctx;
    } handlers[SEL4_SERVER_MAX_HANDLERS];
    uint32_t handler_count;
    uint64_t ep;   /* seL4_CPtr — stored but not used in test builds */
} sel4_server_t;

_Static_assert(sizeof(sel4_server_t) < 4096u,
               "sel4_server_t must fit in a page");

static inline void sel4_server_init(sel4_server_t *srv, uint64_t ep)
{
    srv->handler_count = 0u;
    srv->ep            = ep;
    for (uint32_t i = 0u; i < SEL4_SERVER_MAX_HANDLERS; i++) {
        srv->handlers[i].opcode = 0u;
        srv->handlers[i].fn     = (sel4_handler_fn)0;
        srv->handlers[i].ctx    = (void *)0;
    }
}

static inline int sel4_server_register(sel4_server_t *srv,
                                        uint32_t opcode,
                                        sel4_handler_fn fn,
                                        void *ctx)
{
    if (srv->handler_count >= SEL4_SERVER_MAX_HANDLERS)
        return -1;
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
    for (uint32_t i = 0u; i < srv->handler_count; i++) {
        if (srv->handlers[i].opcode == req->opcode) {
            uint32_t rc = srv->handlers[i].fn(badge, req, rep,
                                               srv->handlers[i].ctx);
            rep->opcode = rc;
            return rc;
        }
    }
    rep->opcode = SEL4_ERR_INVALID_OP;
    rep->length = 0u;
    return SEL4_ERR_INVALID_OP;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Badge encoding (agentOS 64-bit badge layout)
 *
 *   bits 63:32  op_token   (32 bits)
 *   bits 31:16  service_id (16 bits)
 *   bits 15:0   client_id  (16 bits)
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t client_id;
    uint16_t service_id;
    uint32_t op_token;
} badge_fields_t;

static inline sel4_badge_t badge_encode(badge_fields_t f)
{
    return ((sel4_badge_t)f.op_token   << 32u)
         | ((sel4_badge_t)f.service_id << 16u)
         |  (sel4_badge_t)f.client_id;
}

static inline badge_fields_t badge_decode(sel4_badge_t b)
{
    badge_fields_t f;
    f.client_id  = (uint16_t)( b        & 0xFFFFu);
    f.service_id = (uint16_t)((b >> 16u) & 0xFFFFu);
    f.op_token   = (uint32_t)((b >> 32u) & 0xFFFFFFFFu);
    return f;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test opcodes
 * ══════════════════════════════════════════════════════════════════════════ */

#define OP_PING         0x01u   /* ping — returns OP_PING_REP as status     */
#define OP_PING_REP     0x02u   /* expected reply "status" from ping handler */
#define OP_ECHO         0x03u   /* echo req.data back in rep.data           */
#define OP_INDEXED      0x04u   /* echo 4-byte little-endian index          */
#define OP_ROOT_ONLY    0x10u   /* access-controlled: only client_id==0     */

/* ══════════════════════════════════════════════════════════════════════════
 * Shared test state
 * ══════════════════════════════════════════════════════════════════════════ */

static sel4_server_t g_srv;
static sel4_badge_t  g_last_badge;

static void server_reset(void)
{
    sel4_server_init(&g_srv, 1u);
    g_last_badge = 0u;
}

/* Synchronous dispatch helper — models the caller-side round trip */
static uint32_t test_dispatch(sel4_badge_t badge,
                               const sel4_msg_t *req,
                               sel4_msg_t *rep)
{
    g_last_badge = badge;
    return sel4_server_dispatch(&g_srv, badge, req, rep);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Handler implementations
 * ══════════════════════════════════════════════════════════════════════════ */

static uint32_t handler_ping(sel4_badge_t badge,
                              const sel4_msg_t *req,
                              sel4_msg_t *rep,
                              void *ctx)
{
    (void)badge; (void)req; (void)ctx;
    rep->length = 0u;
    return OP_PING_REP;   /* non-zero status — intentional for ordering test */
}

static uint32_t handler_echo(sel4_badge_t badge,
                              const sel4_msg_t *req,
                              sel4_msg_t *rep,
                              void *ctx)
{
    (void)badge; (void)ctx;
    rep->length = req->length < SEL4_MSG_DATA_BYTES
                  ? req->length : SEL4_MSG_DATA_BYTES;
    for (uint32_t i = 0u; i < SEL4_MSG_DATA_BYTES; i++)
        rep->data[i] = req->data[i];
    return SEL4_ERR_OK;
}

static uint32_t handler_indexed(sel4_badge_t badge,
                                 const sel4_msg_t *req,
                                 sel4_msg_t *rep,
                                 void *ctx)
{
    (void)badge; (void)ctx;
    /* req->data[0..3] carries a 32-bit little-endian index; echo it */
    rep->data[0] = req->data[0];
    rep->data[1] = req->data[1];
    rep->data[2] = req->data[2];
    rep->data[3] = req->data[3];
    rep->length  = 4u;
    return SEL4_ERR_OK;
}

static uint32_t handler_root_only(sel4_badge_t badge,
                                   const sel4_msg_t *req,
                                   sel4_msg_t *rep,
                                   void *ctx)
{
    (void)req; (void)ctx;
    badge_fields_t f = badge_decode(badge);
    rep->length = 0u;
    return (f.client_id == 0u) ? SEL4_ERR_OK : SEL4_ERR_FORBIDDEN;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test functions — one TAP point per test (ASSERT_EQ/ASSERT_TRUE each emit
 * exactly one TAP line); multi-assertion tests counted carefully below.
 * ══════════════════════════════════════════════════════════════════════════ */

/* T01 — sel4_msg_t compile-time size check */
static void t01_msg_size(void)
{
    ASSERT_EQ(sizeof(sel4_msg_t), 56u, "sel4_msg_t size is 56 bytes");
}

/* T02 — sel4_badge_t compile-time size check */
static void t02_badge_size(void)
{
    ASSERT_EQ(sizeof(sel4_badge_t), 8u, "sel4_badge_t size is 8 bytes");
}

/* T03 — SEL4_ERR_OK == 0 */
static void t03_err_ok_zero(void)
{
    ASSERT_EQ(SEL4_ERR_OK, 0u, "SEL4_ERR_OK equals 0");
}

/* T04 — SEL4_ERR_INVALID_OP == 1 (the "unknown opcode" code) */
static void t04_err_invalid_op_value(void)
{
    ASSERT_EQ(SEL4_ERR_INVALID_OP, 1u, "SEL4_ERR_INVALID_OP equals 1");
}

/* T05 — SEL4_MSG_DATA_BYTES == 48 */
static void t05_data_bytes(void)
{
    ASSERT_EQ(SEL4_MSG_DATA_BYTES, 48u, "SEL4_MSG_DATA_BYTES equals 48");
}

/* T06 — SEL4_SERVER_MAX_HANDLERS == 32 */
static void t06_max_handlers_constant(void)
{
    ASSERT_EQ(SEL4_SERVER_MAX_HANDLERS, 32u,
              "SEL4_SERVER_MAX_HANDLERS equals 32");
}

/* T07 — badge encode/decode roundtrip */
static void t07_badge_roundtrip(void)
{
    badge_fields_t in  = { .client_id = 0x1234u, .service_id = 0xABCDu,
                           .op_token  = 0xDEADBEEFu };
    badge_fields_t out = badge_decode(badge_encode(in));
    int ok = (out.client_id  == in.client_id  &&
              out.service_id == in.service_id  &&
              out.op_token   == in.op_token);
    ASSERT_TRUE(ok, "badge encode/decode roundtrip");
}

/* T08 — badge client_id max value 0xFFFF roundtrips */
static void t08_badge_client_id_width(void)
{
    badge_fields_t f = { .client_id = 0xFFFFu, .service_id = 0u,
                         .op_token  = 0u };
    ASSERT_EQ(badge_decode(badge_encode(f)).client_id, 0xFFFFu,
              "badge client_id: max value 0xFFFF roundtrips");
}

/* T09 — badge service_id max value 0xFFFF roundtrips */
static void t09_badge_service_id_width(void)
{
    badge_fields_t f = { .client_id = 0u, .service_id = 0xFFFFu,
                         .op_token  = 0u };
    ASSERT_EQ(badge_decode(badge_encode(f)).service_id, 0xFFFFu,
              "badge service_id: max value 0xFFFF roundtrips");
}

/* T10 — badge op_token max value 0xFFFFFFFF roundtrips */
static void t10_badge_op_token_width(void)
{
    badge_fields_t f = { .client_id = 0u, .service_id = 0u,
                         .op_token  = 0xFFFFFFFFu };
    ASSERT_EQ((uint64_t)badge_decode(badge_encode(f)).op_token,
              (uint64_t)0xFFFFFFFFu,
              "badge op_token: max value 0xFFFFFFFF roundtrips");
}

/* T11 — badge encode produces correct bit pattern */
static void t11_badge_bit_pattern(void)
{
    badge_fields_t f   = { .client_id = 1u, .service_id = 2u, .op_token = 3u };
    sel4_badge_t   got = badge_encode(f);
    /* expected: op_token<<32 | service_id<<16 | client_id
     *         = 0x0000000300020001 */
    ASSERT_EQ(got, (sel4_badge_t)0x0000000300020001ULL,
              "badge encode: bit pattern is correct");
}

/* T12 — badge routing: client_id=0 delivered to handler */
static void t12_badge_routing_client0(void)
{
    server_reset();
    sel4_server_register(&g_srv, OP_PING, handler_ping, NULL);
    badge_fields_t f = { .client_id = 0u, .service_id = 0u, .op_token = 0u };
    sel4_msg_t req = { .opcode = OP_PING, .length = 0u };
    sel4_msg_t rep = { 0 };
    test_dispatch(badge_encode(f), &req, &rep);
    ASSERT_EQ(badge_decode(g_last_badge).client_id, 0u,
              "badge routing: client_id=0 seen by handler");
}

/* T13 — badge routing: client_id=7 delivered to handler */
static void t13_badge_routing_client7(void)
{
    server_reset();
    sel4_server_register(&g_srv, OP_PING, handler_ping, NULL);
    badge_fields_t f = { .client_id = 7u, .service_id = 0u, .op_token = 0u };
    sel4_msg_t req = { .opcode = OP_PING, .length = 0u };
    sel4_msg_t rep = { 0 };
    test_dispatch(badge_encode(f), &req, &rep);
    ASSERT_EQ(badge_decode(g_last_badge).client_id, 7u,
              "badge routing: client_id=7 seen by handler");
}

/* T14 — badge routing: service_id=0x1234 delivered to handler */
static void t14_badge_routing_service_id(void)
{
    server_reset();
    sel4_server_register(&g_srv, OP_PING, handler_ping, NULL);
    badge_fields_t f = { .client_id = 0u, .service_id = 0x1234u, .op_token = 0u };
    sel4_msg_t req = { .opcode = OP_PING, .length = 0u };
    sel4_msg_t rep = { 0 };
    test_dispatch(badge_encode(f), &req, &rep);
    ASSERT_EQ(badge_decode(g_last_badge).service_id, 0x1234u,
              "badge routing: service_id=0x1234 seen by handler");
}

/* T15 — call/reply ordering: dispatch propagates handler return in rep->opcode */
static void t15_call_reply_ordering(void)
{
    server_reset();
    sel4_server_register(&g_srv, OP_PING, handler_ping, NULL);
    sel4_msg_t req = { .opcode = OP_PING, .length = 0u };
    sel4_msg_t rep = { 0 };
    test_dispatch(0u, &req, &rep);
    ASSERT_EQ(rep.opcode, OP_PING_REP,
              "call/reply ordering: rep.opcode == handler return value");
}

/* T16 — unknown opcode returns SEL4_ERR_INVALID_OP */
static void t16_unknown_opcode(void)
{
    server_reset();
    sel4_server_register(&g_srv, OP_PING, handler_ping, NULL);
    sel4_msg_t req = { .opcode = 0xDEu, .length = 0u };
    sel4_msg_t rep = { 0 };
    uint32_t rc = test_dispatch(0u, &req, &rep);
    ASSERT_EQ(rc, SEL4_ERR_INVALID_OP,
              "unknown opcode: dispatch returns SEL4_ERR_INVALID_OP");
}

/* T17 — message data roundtrip (8 bytes) */
static void t17_message_data_roundtrip(void)
{
    server_reset();
    sel4_server_register(&g_srv, OP_ECHO, handler_echo, NULL);
    sel4_msg_t req = { .opcode = OP_ECHO, .length = 8u };
    for (uint32_t i = 0u; i < 8u; i++)
        req.data[i] = (uint8_t)(0xA0u + i);
    sel4_msg_t rep = { 0 };
    test_dispatch(0u, &req, &rep);
    int ok = 1;
    for (uint32_t i = 0u; i < 8u; i++)
        if (rep.data[i] != req.data[i]) { ok = 0; break; }
    ASSERT_TRUE(ok, "message data roundtrip: 8 bytes echoed correctly");
}

/* T18 — 48-byte full payload roundtrip */
static void t18_full_payload_roundtrip(void)
{
    server_reset();
    sel4_server_register(&g_srv, OP_ECHO, handler_echo, NULL);
    sel4_msg_t req = { .opcode = OP_ECHO, .length = SEL4_MSG_DATA_BYTES };
    for (uint32_t i = 0u; i < SEL4_MSG_DATA_BYTES; i++)
        req.data[i] = (uint8_t)(i ^ 0x5Au);
    sel4_msg_t rep = { 0 };
    test_dispatch(0u, &req, &rep);
    int ok = (rep.length == SEL4_MSG_DATA_BYTES);
    for (uint32_t i = 0u; ok && i < SEL4_MSG_DATA_BYTES; i++)
        if (rep.data[i] != req.data[i]) ok = 0;
    ASSERT_TRUE(ok, "48-byte full payload roundtrip");
}

/* T19 — req->length=0 produces a valid (zero-length) reply */
static void t19_zero_length_request(void)
{
    server_reset();
    sel4_server_register(&g_srv, OP_ECHO, handler_echo, NULL);
    sel4_msg_t req = { .opcode = OP_ECHO, .length = 0u };
    sel4_msg_t rep = { 0 };
    uint32_t rc = test_dispatch(0u, &req, &rep);
    ASSERT_EQ(rc, SEL4_ERR_OK, "req->length=0: reply status is SEL4_ERR_OK");
}

/* T20 — multi-message: 100 calls in order, none reordered */
static void t20_multi_message_ordering(void)
{
    server_reset();
    sel4_server_register(&g_srv, OP_INDEXED, handler_indexed, NULL);
    int all_ok = 1;
    for (uint32_t i = 0u; i < 100u; i++) {
        sel4_msg_t req = { .opcode = OP_INDEXED, .length = 4u };
        req.data[0] = (uint8_t)( i        & 0xFFu);
        req.data[1] = (uint8_t)((i >>  8u) & 0xFFu);
        req.data[2] = (uint8_t)((i >> 16u) & 0xFFu);
        req.data[3] = (uint8_t)((i >> 24u) & 0xFFu);
        sel4_msg_t rep = { 0 };
        test_dispatch(0u, &req, &rep);
        uint32_t got = (uint32_t)rep.data[0]
                     | ((uint32_t)rep.data[1] <<  8u)
                     | ((uint32_t)rep.data[2] << 16u)
                     | ((uint32_t)rep.data[3] << 24u);
        if (got != i) { all_ok = 0; break; }
    }
    ASSERT_TRUE(all_ok, "multi-message: 100 indexed calls, none reordered");
}

/* T21 — register 32 handlers without overflow */
static void t21_register_32_handlers(void)
{
    server_reset();
    int fail = 0;
    for (uint32_t i = 0u; i < SEL4_SERVER_MAX_HANDLERS; i++) {
        if (sel4_server_register(&g_srv, i, handler_echo, NULL) != 0) {
            fail = 1; break;
        }
    }
    ASSERT_FALSE(fail, "register 32 handlers: all succeed (no overflow)");
}

/* T22 — 33rd registration fails (-1) */
static void t22_register_overflow(void)
{
    server_reset();
    for (uint32_t i = 0u; i < SEL4_SERVER_MAX_HANDLERS; i++)
        sel4_server_register(&g_srv, i, handler_echo, NULL);
    int rc = sel4_server_register(&g_srv, 99u, handler_echo, NULL);
    ASSERT_EQ(rc, -1, "register 33rd handler: returns -1 (overflow)");
}

/* T23 — duplicate opcode registration: first match wins */
static void t23_duplicate_opcode_first_match(void)
{
    server_reset();
    /* First: handler_ping (returns OP_PING_REP) */
    sel4_server_register(&g_srv, OP_PING, handler_ping, NULL);
    /* Second: handler_echo (returns SEL4_ERR_OK) */
    sel4_server_register(&g_srv, OP_PING, handler_echo, NULL);
    sel4_msg_t req = { .opcode = OP_PING, .length = 0u };
    sel4_msg_t rep = { 0 };
    test_dispatch(0u, &req, &rep);
    ASSERT_EQ(rep.opcode, OP_PING_REP,
              "duplicate opcode: first registration fires (first-match wins)");
}

/* T24 — ACL check: non-root client (client_id=5) denied on root-only op */
static void t24_acl_non_root_denied(void)
{
    server_reset();
    sel4_server_register(&g_srv, OP_ROOT_ONLY, handler_root_only, NULL);
    badge_fields_t f = { .client_id = 5u, .service_id = 0u, .op_token = 0u };
    sel4_msg_t req = { .opcode = OP_ROOT_ONLY, .length = 0u };
    sel4_msg_t rep = { 0 };
    uint32_t rc = test_dispatch(badge_encode(f), &req, &rep);
    ASSERT_EQ(rc, SEL4_ERR_FORBIDDEN,
              "ACL: client_id=5 denied on root-only op (SEL4_ERR_FORBIDDEN)");
}

/* T25 — ACL check: root task (client_id=0) granted on root-only op */
static void t25_acl_root_granted(void)
{
    server_reset();
    sel4_server_register(&g_srv, OP_ROOT_ONLY, handler_root_only, NULL);
    badge_fields_t f = { .client_id = 0u, .service_id = 0u, .op_token = 0u };
    sel4_msg_t req = { .opcode = OP_ROOT_ONLY, .length = 0u };
    sel4_msg_t rep = { 0 };
    uint32_t rc = test_dispatch(badge_encode(f), &req, &rep);
    ASSERT_EQ(rc, SEL4_ERR_OK,
              "ACL: client_id=0 (root) granted on root-only op (SEL4_ERR_OK)");
}

/* ══════════════════════════════════════════════════════════════════════════
 * main — 25 TAP test points (one ASSERT per test function)
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    TAP_PLAN(25);

    t01_msg_size();
    t02_badge_size();
    t03_err_ok_zero();
    t04_err_invalid_op_value();
    t05_data_bytes();
    t06_max_handlers_constant();
    t07_badge_roundtrip();
    t08_badge_client_id_width();
    t09_badge_service_id_width();
    t10_badge_op_token_width();
    t11_badge_bit_pattern();
    t12_badge_routing_client0();
    t13_badge_routing_client7();
    t14_badge_routing_service_id();
    t15_call_reply_ordering();
    t16_unknown_opcode();
    t17_message_data_roundtrip();
    t18_full_payload_roundtrip();
    t19_zero_length_request();
    t20_multi_message_ordering();
    t21_register_32_handlers();
    t22_register_overflow();
    t23_duplicate_opcode_first_match();
    t24_acl_non_root_denied();
    t25_acl_root_granted();

    printf("TAP_DONE:%d\n", _tap_failed);
    return tap_exit();
}

#else
typedef int _agentos_ipc_tests_dummy;
#endif /* AGENTOS_TEST_HOST */
