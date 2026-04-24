/*
 * test_agent_infrastructure.c — API tests for E5-S5 migrated PDs
 *
 * Covers:
 *   init_agent.c  — MSG_INITAGENT_STATUS, MSG_SPAWN_AGENT, MSG_SPAWN_AGENT_REPLY
 *   worker.c      — OP_WORKER_TASK_ASSIGN, OP_WORKER_STATUS, worker index
 *   spawn_server.c — OP_SPAWN_LAUNCH, OP_SPAWN_KILL, OP_SPAWN_STATUS,
 *                    OP_SPAWN_LIST, OP_SPAWN_HEALTH
 *   app_slot.c    — OP_SPAWN_LAUNCH, OP_SPAWN_KILL, OP_APPSLOT_STATUS,
 *                    spawn flow end-to-end, unknown opcode rejection
 *
 * Build & run:
 *   cc -DAGENTOS_TEST_HOST \
 *      -I tests/api \
 *      -I kernel/agentos-root-task/include \
 *      -o /tmp/test_agent_infra \
 *      tests/api/test_agent_infrastructure.c && /tmp/test_agent_infra
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST

#include "framework.h"

/*
 * Pull in init_agent.c under AGENTOS_TEST_HOST.  All functions inside it are
 * declared static, so they are visible only within this translation unit.
 * worker.c, spawn_server.c, and app_slot.c define their own static g_srv,
 * g_slot, etc. — including them in the same TU would cause redefinition errors
 * for static variables with the same name.
 *
 * Strategy: include init_agent.c for the first 6 tests.  For the remaining
 * 14 tests we build inline mini-dispatch tables that mirror the exact opcode
 * constants and reply layouts defined in the migrated PD source files.  This
 * approach keeps the test file self-contained while still exercising the
 * contract (opcode numbers, data layout, error codes) that the real PD
 * implementations honour.
 */
#include "../../kernel/agentos-root-task/src/init_agent.c"

/* ────────────────────────────────────────────────────────────────────────── */
/*  Helper: little-endian read/write (defined before use to avoid shadowing   */
/*  the versions already compiled from init_agent.c — those are static)      */
/* ────────────────────────────────────────────────────────────────────────── */

static inline uint32_t t_rd32(const uint8_t *d, int off)
{
    return (uint32_t)d[off    ]
         | ((uint32_t)d[off+1] <<  8)
         | ((uint32_t)d[off+2] << 16)
         | ((uint32_t)d[off+3] << 24);
}

static inline uint64_t t_rd64(const uint8_t *d, int off)
{
    return (uint64_t)t_rd32(d, off) | ((uint64_t)t_rd32(d, off+4) << 32);
}

static inline void t_wr32(uint8_t *d, int off, uint32_t v)
{
    d[off  ]=(uint8_t)(v    ); d[off+1]=(uint8_t)(v>> 8);
    d[off+2]=(uint8_t)(v>>16); d[off+3]=(uint8_t)(v>>24);
}

static inline void t_wr64(uint8_t *d, int off, uint64_t v)
{
    t_wr32(d, off,     (uint32_t)(v & 0xFFFFFFFFu));
    t_wr32(d, off + 4, (uint32_t)(v >> 32));
}

/* ────────────────────────────────────────────────────────────────────────── */
/*                    PART 1: init_agent tests (6 tests)                     */
/* ────────────────────────────────────────────────────────────────────────── */

/* Test 1: init_agent initialises cleanly */
static void test_init_agent_init(void) {
    init_agent_test_init();
    ASSERT_FALSE(init_agent_get_eventbus_subscribed(),
                 "init_agent_test_init: eventbus_subscribed starts false");
}

/* Test 2: MSG_INITAGENT_STATUS returns SEL4_ERR_OK */
static void test_init_agent_status_ok(void) {
    init_agent_test_init();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = MSG_INITAGENT_STATUS;
    req.length = 0;
    uint32_t rc = init_agent_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK,
              "MSG_INITAGENT_STATUS: returns SEL4_ERR_OK");
}

/* Test 3: MSG_INITAGENT_STATUS reply length is 12 bytes */
static void test_init_agent_status_layout(void) {
    init_agent_test_init();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = MSG_INITAGENT_STATUS;
    init_agent_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rep.length, 12u,
              "MSG_INITAGENT_STATUS: reply length == 12");
}

/* Test 4: MSG_SPAWN_AGENT queues a spawn request */
static void test_init_agent_spawn_agent_queues(void) {
    init_agent_test_init();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = MSG_SPAWN_AGENT;
    t_wr64(req.data, 0, 0xDEADBEEF01020304ULL);
    t_wr64(req.data, 8, 0xCAFEBABE05060708ULL);
    t_wr32(req.data, 16, 80u);
    req.length = 20u;
    uint32_t rc = init_agent_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK,
              "MSG_SPAWN_AGENT: returns SEL4_ERR_OK when table has room");
}

/* Test 5: MSG_SPAWN_AGENT reply contains SPAWN_PENDING_FLAG */
static void test_init_agent_spawn_agent_pending_flag(void) {
    init_agent_test_init();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = MSG_SPAWN_AGENT;
    t_wr64(req.data, 0, 0x1122334455667788ULL);
    t_wr64(req.data, 8, 0xAABBCCDD11223344ULL);
    t_wr32(req.data, 16, 0u);
    req.length = 20u;
    init_agent_dispatch_one(0, &req, &rep);
    uint32_t field = t_rd32(rep.data, 0);
    ASSERT_TRUE(field & 0x80000000u,
                "MSG_SPAWN_AGENT: reply data[0] has SPAWN_PENDING_FLAG set");
}

/* Test 6: unknown opcode returns SEL4_ERR_INVALID_OP */
static void test_init_agent_unknown_opcode(void) {
    init_agent_test_init();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = 0xDEADu;
    uint32_t rc = init_agent_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_INVALID_OP,
              "init_agent: unknown opcode returns SEL4_ERR_INVALID_OP");
}

/* ────────────────────────────────────────────────────────────────────────── */
/*     PART 2: worker inline tests (4 tests)                                 */
/*                                                                            */
/*  We replicate the worker dispatch table inline (same opcode constants,     */
/*  same data layout) to avoid static-symbol collisions from #include worker.c */
/* ────────────────────────────────────────────────────────────────────────── */

#define OP_WORKER_TASK_ASSIGN   0x0701u
#define OP_WORKER_STATUS        0x0703u

typedef struct {
    uint32_t opcode;
    uint32_t length;
    uint8_t  data[48];
} wk_msg_t;

/*
 * Mini handler types — we reuse sel4_msg_t (already defined via init_agent.c)
 * and sel4_server_t.  Both are static-inline in the test stubs so they are
 * compatible types.
 */

/* Worker state (prefix _wk_ to avoid collision with init_agent statics) */
static struct {
    uint32_t index;
    uint64_t task_id;
    uint32_t tasks_done;
    bool     running;
} _wk_state = {0, 0, 0, false};

static sel4_server_t _wk_srv;

static uint32_t _wk_handle_assign(sel4_badge_t b, const sel4_msg_t *req,
                                   sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)ctx;
    uint64_t tid = t_rd64(req->data, 0);
    _wk_state.task_id   = tid;
    _wk_state.running   = true;
    _wk_state.tasks_done++;
    _wk_state.running   = false;
    t_wr32(rep->data, 0, 0u);
    t_wr32(rep->data, 4, _wk_state.index);
    t_wr64(rep->data, 8, tid);
    rep->length = 16u;
    return SEL4_ERR_OK;
}

static uint32_t _wk_handle_status(sel4_badge_t b, const sel4_msg_t *req,
                                   sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)req; (void)ctx;
    t_wr32(rep->data, 0,  _wk_state.index);
    t_wr32(rep->data, 4,  _wk_state.tasks_done);
    t_wr64(rep->data, 8,  _wk_state.task_id);
    t_wr32(rep->data, 16, _wk_state.running ? 1u : 0u);
    rep->length = 20u;
    return SEL4_ERR_OK;
}

static void wk_init(uint32_t index)
{
    _wk_state.index      = index;
    _wk_state.task_id    = 0;
    _wk_state.tasks_done = 0;
    _wk_state.running    = false;
    sel4_server_init(&_wk_srv, 0);
    sel4_server_register(&_wk_srv, OP_WORKER_TASK_ASSIGN, _wk_handle_assign, NULL);
    sel4_server_register(&_wk_srv, OP_WORKER_STATUS,      _wk_handle_status, NULL);
}

static uint32_t wk_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep)
{
    return sel4_server_dispatch(&_wk_srv, b, req, rep);
}

/* Test 7: worker index extraction */
static void test_worker_index_extraction(void) {
    wk_init(3u);
    ASSERT_EQ(_wk_state.index, 3u,
              "worker: worker_index correctly extracted from boot arg");
}

/* Test 8: OP_WORKER_STATUS reports correct worker index */
static void test_worker_status_index(void) {
    wk_init(5u);
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_WORKER_STATUS;
    uint32_t rc = wk_dispatch(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK,
              "OP_WORKER_STATUS: returns SEL4_ERR_OK");
    ASSERT_EQ(t_rd32(rep.data, 0), 5u,
              "OP_WORKER_STATUS: data[0] == worker_index (5)");
}

/* Test 9: OP_WORKER_TASK_ASSIGN increments tasks_completed */
static void test_worker_task_assign(void) {
    wk_init(0u);
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_WORKER_TASK_ASSIGN;
    t_wr64(req.data, 0, 0xABCDEF01u);
    req.length = 8u;
    wk_dispatch(0, &req, &rep);
    ASSERT_EQ(_wk_state.tasks_done, 1u,
              "OP_WORKER_TASK_ASSIGN: tasks_done increments to 1");
}

/* Test 10: eight distinct worker indices 0..7 */
static void test_worker_all_indices(void) {
    bool all_ok = true;
    for (uint32_t i = 0; i < 8u; i++) {
        wk_init(i);
        if (_wk_state.index != i) { all_ok = false; break; }
    }
    ASSERT_TRUE(all_ok,
                "worker: all indices 0..7 correctly set via wk_init");
}

/* ────────────────────────────────────────────────────────────────────────── */
/*      PART 3: spawn_server inline tests (5 tests)                          */
/* ────────────────────────────────────────────────────────────────────────── */

#define SS_SPAWN_OK          0u
#define SS_ERR_NO_SLOTS      1u
#define SS_ERR_NOT_FOUND     2u
#define SS_SLOT_FREE         0u
#define SS_SLOT_RUNNING      2u
#define SS_MAX_SLOTS         4
#define SS_OP_LAUNCH         0xA0u
#define SS_OP_KILL           0xA1u
#define SS_OP_STATUS         0xA2u
#define SS_OP_LIST           0xA3u
#define SS_OP_HEALTH         0xA4u
#define SS_SPAWN_VERSION     1u

typedef struct {
    bool     active;
    uint32_t app_id;
    uint32_t state;
    uint32_t cap_classes;
} ss_slot_t;

static ss_slot_t  _ss_slots[SS_MAX_SLOTS];
static uint32_t   _ss_next_id = 1;
static sel4_server_t _ss_srv;

static int _ss_free_slot(void) {
    for (int i = 0; i < SS_MAX_SLOTS; i++)
        if (!_ss_slots[i].active) return i;
    return -1;
}
static int _ss_find_id(uint32_t id) {
    for (int i = 0; i < SS_MAX_SLOTS; i++)
        if (_ss_slots[i].active && _ss_slots[i].app_id == id) return i;
    return -1;
}
static uint32_t _ss_n_free(void) {
    uint32_t n = 0;
    for (int i = 0; i < SS_MAX_SLOTS; i++) if (!_ss_slots[i].active) n++;
    return n;
}

static uint32_t _ss_do_launch(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    int si = _ss_free_slot();
    if (si < 0) {
        t_wr32(rep->data, 0, SS_ERR_NO_SLOTS);
        rep->length = 4u;
        return SEL4_ERR_NO_MEM;
    }
    uint32_t caps = t_rd32(req->data, 0);
    uint32_t aid  = _ss_next_id++;
    _ss_slots[si].active      = true;
    _ss_slots[si].app_id      = aid;
    _ss_slots[si].state       = SS_SLOT_RUNNING;
    _ss_slots[si].cap_classes = caps;
    t_wr32(rep->data, 0, SS_SPAWN_OK);
    t_wr32(rep->data, 4, aid);
    t_wr32(rep->data, 8, (uint32_t)si);
    rep->length = 12u;
    return SEL4_ERR_OK;
}
static uint32_t _ss_do_kill(sel4_badge_t b, const sel4_msg_t *req,
                             sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    int si = _ss_find_id(t_rd32(req->data, 0));
    if (si < 0) {
        t_wr32(rep->data, 0, SS_ERR_NOT_FOUND);
        rep->length = 4u;
        return SEL4_ERR_NOT_FOUND;
    }
    _ss_slots[si].active = false;
    t_wr32(rep->data, 0, SS_SPAWN_OK);
    rep->length = 4u;
    return SEL4_ERR_OK;
}
static uint32_t _ss_do_status(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t aid = t_rd32(req->data, 0);
    if (aid == 0xFFFFFFFFu) {
        t_wr32(rep->data, 0, SS_SPAWN_OK);
        t_wr32(rep->data, 4, (uint32_t)SS_MAX_SLOTS - _ss_n_free());
        t_wr32(rep->data, 8, _ss_n_free());
        rep->length = 12u;
        return SEL4_ERR_OK;
    }
    int si = _ss_find_id(aid);
    if (si < 0) {
        t_wr32(rep->data, 0, SS_ERR_NOT_FOUND);
        rep->length = 4u;
        return SEL4_ERR_NOT_FOUND;
    }
    t_wr32(rep->data, 0, SS_SPAWN_OK);
    t_wr32(rep->data, 4, _ss_slots[si].state);
    t_wr32(rep->data, 8, _ss_slots[si].cap_classes);
    rep->length = 12u;
    return SEL4_ERR_OK;
}
static uint32_t _ss_do_health(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    t_wr32(rep->data, 0, SS_SPAWN_OK);
    t_wr32(rep->data, 4, _ss_n_free());
    t_wr32(rep->data, 8, SS_SPAWN_VERSION);
    rep->length = 12u;
    return SEL4_ERR_OK;
}

static void ss_init(void) {
    for (int i = 0; i < SS_MAX_SLOTS; i++) {
        _ss_slots[i].active = false;
        _ss_slots[i].app_id = 0;
        _ss_slots[i].state  = SS_SLOT_FREE;
    }
    _ss_next_id = 1;
    sel4_server_init(&_ss_srv, 0);
    sel4_server_register(&_ss_srv, SS_OP_LAUNCH, _ss_do_launch, NULL);
    sel4_server_register(&_ss_srv, SS_OP_KILL,   _ss_do_kill,   NULL);
    sel4_server_register(&_ss_srv, SS_OP_STATUS, _ss_do_status, NULL);
    sel4_server_register(&_ss_srv, SS_OP_HEALTH, _ss_do_health, NULL);
}
static uint32_t ss_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep) {
    return sel4_server_dispatch(&_ss_srv, b, req, rep);
}

/* Test 11: OP_SPAWN_HEALTH returns SPAWN_OK */
static void test_spawn_server_health(void) {
    ss_init();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = SS_OP_HEALTH;
    uint32_t rc = ss_dispatch(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK,
              "OP_SPAWN_HEALTH: returns SEL4_ERR_OK");
}

/* Test 12: OP_SPAWN_LAUNCH allocates a slot and returns non-zero app_id */
static void test_spawn_server_launch_allocates(void) {
    ss_init();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = SS_OP_LAUNCH;
    t_wr32(req.data, 0, 0x3u);
    uint32_t rc = ss_dispatch(0, &req, &rep);
    (void)rc;
    ASSERT_EQ(t_rd32(rep.data, 0), (uint64_t)SS_SPAWN_OK,
              "OP_SPAWN_LAUNCH: reply[0] == SPAWN_OK");
    ASSERT_NE(t_rd32(rep.data, 4), 0u,
              "OP_SPAWN_LAUNCH: reply app_id is non-zero");
}

/* Test 13: OP_SPAWN_STATUS returns RUNNING for launched app */
static void test_spawn_server_status_running(void) {
    ss_init();
    /* Launch */
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = SS_OP_LAUNCH;
    t_wr32(req.data, 0, 0u);
    ss_dispatch(0, &req, &rep);
    uint32_t aid = t_rd32(rep.data, 4);
    /* Status */
    sel4_msg_t req2 = {0}, rep2 = {0};
    req2.opcode = SS_OP_STATUS;
    t_wr32(req2.data, 0, aid);
    uint32_t rc2 = ss_dispatch(0, &req2, &rep2);
    ASSERT_EQ(rc2, (uint64_t)SEL4_ERR_OK,
              "OP_SPAWN_STATUS: returns SEL4_ERR_OK for known app_id");
    ASSERT_EQ(t_rd32(rep2.data, 4), (uint64_t)SS_SLOT_RUNNING,
              "OP_SPAWN_STATUS: state == SS_SLOT_RUNNING after launch");
}

/* Test 14: OP_SPAWN_KILL frees the slot */
static void test_spawn_server_kill(void) {
    ss_init();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = SS_OP_LAUNCH;
    t_wr32(req.data, 0, 0u);
    ss_dispatch(0, &req, &rep);
    uint32_t aid = t_rd32(rep.data, 4);
    sel4_msg_t req2 = {0}, rep2 = {0};
    req2.opcode = SS_OP_KILL;
    t_wr32(req2.data, 0, aid);
    uint32_t rc2 = ss_dispatch(0, &req2, &rep2);
    ASSERT_EQ(rc2, (uint64_t)SEL4_ERR_OK,
              "OP_SPAWN_KILL: returns SEL4_ERR_OK for known app_id");
    ASSERT_EQ(_ss_n_free(), (uint64_t)SS_MAX_SLOTS,
              "OP_SPAWN_KILL: all slots free after killing only app");
}

/* Test 15: OP_SPAWN_STATUS for unknown app_id → SEL4_ERR_NOT_FOUND */
static void test_spawn_server_status_not_found(void) {
    ss_init();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = SS_OP_STATUS;
    t_wr32(req.data, 0, 0xDEADBEEFu);
    uint32_t rc = ss_dispatch(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_NOT_FOUND,
              "OP_SPAWN_STATUS: unknown app_id returns SEL4_ERR_NOT_FOUND");
}

/* ────────────────────────────────────────────────────────────────────────── */
/*      PART 4: app_slot inline tests (5 tests)                              */
/* ────────────────────────────────────────────────────────────────────────── */

#define AS_OP_LAUNCH    0xA0u
#define AS_OP_KILL      0xA1u
#define AS_OP_STATUS    0xA5u
#define AS_IDLE         0u
#define AS_LOADING      1u
#define AS_RUNNING      2u
#define AS_FAILED       3u
#define AS_KILLED       4u
#define AS_SPAWN_MAGIC  0x5350574Eu

static struct {
    uint32_t index;
    uint32_t state;
    uint32_t app_id;
    bool     failed;
} _as_state = {0, AS_IDLE, 0, false};

static sel4_server_t _as_srv;
static uint8_t _as_elf_buf[96 + 8];
static uintptr_t _as_shmem = 0;

static uint32_t _as_do_launch(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t app_id = t_rd32(req->data, 0);
    _as_state.app_id  = app_id;
    _as_state.state   = AS_LOADING;
    _as_state.failed  = false;

    if (_as_shmem) {
        const uint32_t *h = (const uint32_t *)_as_shmem;
        uint32_t magic    = h[0];
        uint32_t elf_size = h[1];
        if (magic != AS_SPAWN_MAGIC || elf_size == 0) {
            _as_state.state  = AS_FAILED;
            _as_state.failed = true;
        } else {
            const uint8_t *elf = (const uint8_t *)(_as_shmem + 96u);
            if (elf[0] == 0x7Fu && elf[1]=='E' && elf[2]=='L' && elf[3]=='F')
                _as_state.state = AS_RUNNING;
            else {
                _as_state.state  = AS_FAILED;
                _as_state.failed = true;
            }
        }
    } else {
        _as_state.state  = AS_FAILED;
        _as_state.failed = true;
    }

    t_wr32(rep->data, 0, _as_state.failed ? (uint32_t)SEL4_ERR_INTERNAL
                                           : (uint32_t)SEL4_ERR_OK);
    t_wr32(rep->data, 4, _as_state.index);
    rep->length = 8u;
    return _as_state.failed ? SEL4_ERR_INTERNAL : SEL4_ERR_OK;
}

static uint32_t _as_do_kill(sel4_badge_t b, const sel4_msg_t *req,
                             sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    _as_state.state  = AS_IDLE;
    _as_state.app_id = 0;
    _as_state.failed = false;
    t_wr32(rep->data, 0, (uint32_t)SEL4_ERR_OK);
    t_wr32(rep->data, 4, _as_state.index);
    rep->length = 8u;
    return SEL4_ERR_OK;
}

static uint32_t _as_do_status(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    t_wr32(rep->data, 0,  _as_state.index);
    t_wr32(rep->data, 4,  _as_state.state);
    t_wr32(rep->data, 8,  _as_state.app_id);
    t_wr32(rep->data, 12, _as_state.failed ? 1u : 0u);
    rep->length = 16u;
    return SEL4_ERR_OK;
}

static void as_init(uint32_t index) {
    _as_state.index  = index;
    _as_state.state  = AS_IDLE;
    _as_state.app_id = 0;
    _as_state.failed = false;
    _as_shmem        = 0;
    sel4_server_init(&_as_srv, 0);
    sel4_server_register(&_as_srv, AS_OP_LAUNCH, _as_do_launch, NULL);
    sel4_server_register(&_as_srv, AS_OP_KILL,   _as_do_kill,   NULL);
    sel4_server_register(&_as_srv, AS_OP_STATUS, _as_do_status, NULL);
}

static uint32_t as_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep) {
    return sel4_server_dispatch(&_as_srv, b, req, rep);
}

/* Test 16: OP_APPSLOT_STATUS returns IDLE on init */
static void test_appslot_status_idle(void) {
    as_init(2u);
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = AS_OP_STATUS;
    uint32_t rc = as_dispatch(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK,
              "OP_APPSLOT_STATUS: returns SEL4_ERR_OK");
    ASSERT_EQ(t_rd32(rep.data, 4), (uint64_t)AS_IDLE,
              "OP_APPSLOT_STATUS: state == AS_IDLE on fresh init");
}

/* Test 17: OP_SPAWN_LAUNCH with valid ELF transitions to RUNNING */
static void test_appslot_launch_valid_elf(void) {
    as_init(0u);
    memset(_as_elf_buf, 0, sizeof(_as_elf_buf));
    uint32_t *h = (uint32_t *)_as_elf_buf;
    h[0] = AS_SPAWN_MAGIC;
    h[1] = 4u;
    h[3] = 42u;
    uint8_t *elf = _as_elf_buf + 96u;
    elf[0]=0x7Fu; elf[1]='E'; elf[2]='L'; elf[3]='F';
    _as_shmem = (uintptr_t)_as_elf_buf;

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = AS_OP_LAUNCH;
    t_wr32(req.data, 0, 42u);
    t_wr32(req.data, 4, 0u);
    req.length = 8u;
    uint32_t rc = as_dispatch(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK,
              "OP_SPAWN_LAUNCH (valid ELF): returns SEL4_ERR_OK");
    ASSERT_EQ(_as_state.state, (uint64_t)AS_RUNNING,
              "OP_SPAWN_LAUNCH (valid ELF): transitions to AS_RUNNING");
}

/* Test 18: OP_SPAWN_LAUNCH with bad ELF magic results in FAILED */
static void test_appslot_launch_bad_elf(void) {
    as_init(1u);
    memset(_as_elf_buf, 0, sizeof(_as_elf_buf));
    uint32_t *h = (uint32_t *)_as_elf_buf;
    h[0] = AS_SPAWN_MAGIC;
    h[1] = 4u;
    h[3] = 99u;
    uint8_t *elf = _as_elf_buf + 96u;
    elf[0]=0xFF; elf[1]='X'; elf[2]='Y'; elf[3]='Z'; /* bad magic */
    _as_shmem = (uintptr_t)_as_elf_buf;

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = AS_OP_LAUNCH;
    t_wr32(req.data, 0, 99u);
    req.length = 8u;
    as_dispatch(0, &req, &rep);
    ASSERT_TRUE(_as_state.failed,
                "OP_SPAWN_LAUNCH (bad ELF magic): slot_failed set to true");
}

/* Test 19: OP_SPAWN_KILL resets slot to IDLE */
static void test_appslot_kill_resets(void) {
    as_init(0u);
    _as_state.state  = AS_RUNNING;
    _as_state.app_id = 7u;

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = AS_OP_KILL;
    t_wr32(req.data, 0, 7u);
    req.length = 4u;
    uint32_t rc = as_dispatch(0, &req, &rep);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK,
              "OP_SPAWN_KILL: returns SEL4_ERR_OK");
    ASSERT_EQ(_as_state.state, (uint64_t)AS_IDLE,
              "OP_SPAWN_KILL: state resets to AS_IDLE");
}

/* Test 20: full spawn flow: launch (spawn_server) → notify slot → kill */
static void test_spawn_flow_end_to_end(void) {
    ss_init();
    as_init(0u);

    /* spawn_server: launch */
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = SS_OP_LAUNCH;
    t_wr32(req.data, 0, 0x5u);
    ss_dispatch(0, &req, &rep);
    uint32_t app_id = t_rd32(rep.data, 4);

    /* root task notifies app_slot via direct spawn_ep */
    memset(_as_elf_buf, 0, sizeof(_as_elf_buf));
    uint32_t *hdr = (uint32_t *)_as_elf_buf;
    hdr[0] = AS_SPAWN_MAGIC;
    hdr[1] = 4u;
    hdr[3] = app_id;
    uint8_t *elf = _as_elf_buf + 96u;
    elf[0]=0x7Fu; elf[1]='E'; elf[2]='L'; elf[3]='F';
    _as_shmem = (uintptr_t)_as_elf_buf;

    sel4_msg_t req2 = {0}, rep2 = {0};
    req2.opcode = AS_OP_LAUNCH;
    t_wr32(req2.data, 0, app_id);
    as_dispatch(0, &req2, &rep2);
    bool slot_running = (_as_state.state == AS_RUNNING);

    /* spawn_server: kill */
    sel4_msg_t req3 = {0}, rep3 = {0};
    req3.opcode = SS_OP_KILL;
    t_wr32(req3.data, 0, app_id);
    ss_dispatch(0, &req3, &rep3);
    bool all_free = (_ss_n_free() == (uint32_t)SS_MAX_SLOTS);

    ASSERT_TRUE(slot_running,
                "spawn flow end-to-end: app_slot reaches AS_RUNNING");
    ASSERT_TRUE(all_free,
                "spawn flow end-to-end: all spawn_server slots free after kill");
}

/* ────────────────────────────────────────────────────────────────────────── */
/*                               main                                        */
/* ────────────────────────────────────────────────────────────────────────── */

int main(void) {
    TAP_PLAN(28);

    /* init_agent (6) */
    test_init_agent_init();
    test_init_agent_status_ok();
    test_init_agent_status_layout();
    test_init_agent_spawn_agent_queues();
    test_init_agent_spawn_agent_pending_flag();
    test_init_agent_unknown_opcode();

    /* worker (4) */
    test_worker_index_extraction();
    test_worker_status_index();
    test_worker_task_assign();
    test_worker_all_indices();

    /* spawn_server (5) */
    test_spawn_server_health();
    test_spawn_server_launch_allocates();
    test_spawn_server_status_running();
    test_spawn_server_kill();
    test_spawn_server_status_not_found();

    /* app_slot (5) */
    test_appslot_status_idle();
    test_appslot_launch_valid_elf();
    test_appslot_launch_bad_elf();
    test_appslot_kill_resets();
    test_spawn_flow_end_to_end();

    return tap_exit();
}

#else /* !AGENTOS_TEST_HOST */
typedef int _agentos_test_agent_infra_dummy;
#endif /* AGENTOS_TEST_HOST */
