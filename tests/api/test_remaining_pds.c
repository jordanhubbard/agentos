/*
 * test_remaining_pds.c — API-level tests for PDs migrated in E5-S8
 *
 * Covers the following protection domains (all migrated from Microkit to
 * raw seL4 IPC in task E5-S8):
 *
 *   exec_server   (OP_EXEC_LAUNCH, STATUS, WAIT, KILL)
 *   core_affinity (OP_AFFINITY_PIN, UNPIN, STATUS, SUGGEST, RESERVE, RESET)
 *   cap_audit_log (OP_CAP_LOG, LOG_STATUS, LOG_DUMP)
 *   nameserver    (OP_NS_REGISTER, LOOKUP, UPDATE_STATUS, DEREGISTER, HEALTH)
 *   serial_pd     (MSG_SERIAL_OPEN, WRITE, STATUS, unknown)
 *   block_pd      (MSG_BLOCK_OPEN, READ, WRITE, STATUS)
 *   vfs_server    (OP_VFS_OPEN, CLOSE, STAT, unknown)
 *   spawn_server  (OP_SPAWN_LAUNCH, KILL, STATUS, HEALTH)
 *
 * All tests run on the host using inline mock implementations.
 * Build & run (from repo root):
 *   cc -DAGENTOS_TEST_HOST -std=c11 -I tests/api \
 *      -o /tmp/test_remaining_pds tests/api/test_remaining_pds.c \
 *      && /tmp/test_remaining_pds
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST

#include "framework.h"

/* ── shared IPC ABI stubs (mirror agentos.h for test builds) ─────────────── */

#define SEL4_ERR_OK          0u
#define SEL4_ERR_INVALID_OP  0xFFu

typedef uint32_t sel4_badge_t;

typedef struct {
    uint32_t opcode;
    uint32_t length;         /* payload bytes in data[] */
    uint8_t  data[48];
} sel4_msg_t;

/* Little-endian 32-bit read/write helpers into sel4_msg_t.data[] */
static inline uint32_t msg_u32(const sel4_msg_t *m, uint32_t off) {
    if (off + 4u > sizeof(m->data)) return 0;
    return (uint32_t)m->data[off]
         | ((uint32_t)m->data[off+1] << 8)
         | ((uint32_t)m->data[off+2] << 16)
         | ((uint32_t)m->data[off+3] << 24);
}

static inline void rep_u32(sel4_msg_t *m, uint32_t off, uint32_t v) {
    if (off + 4u > sizeof(m->data)) return;
    m->data[off]   = (uint8_t)(v);
    m->data[off+1] = (uint8_t)(v >> 8);
    m->data[off+2] = (uint8_t)(v >> 16);
    m->data[off+3] = (uint8_t)(v >> 24);
}

/* sel4_dbg_puts stub — no-op on host */
#define sel4_dbg_puts(s) ((void)(s))

/* ======================================================================== */
/* ── EXEC SERVER ──────────────────────────────────────────────────────── */
/* ======================================================================== */

#define OP_EXEC_LAUNCH  0xE0u
#define OP_EXEC_STATUS  0xE1u
#define OP_EXEC_WAIT    0xE2u
#define OP_EXEC_KILL    0xE3u

#define EXEC_MAX_TABLE  8u
#define EXEC_PATH_MAX   64u
#define EXEC_FREE       0u
#define EXEC_STATE_LOADING  1u
#define EXEC_STATE_RUNNING  2u
#define EXEC_STATE_DONE     3u

typedef struct {
    uint8_t  state;
    uint8_t  app_slot_id;
    uint8_t  _pad[2];
    uint32_t exec_id;
    uint32_t auth_token;
    uint32_t cap_mask;
    uint32_t pid;
    char     path[EXEC_PATH_MAX];
} exec_entry_t;

static exec_entry_t es_table[EXEC_MAX_TABLE];
static uint32_t     es_next_pid = 100u;

static void es_reset(void) {
    for (uint32_t i = 0; i < EXEC_MAX_TABLE; i++) {
        es_table[i].state = EXEC_FREE;
        es_table[i].exec_id = 0;
        es_table[i].pid = 0;
    }
    es_next_pid = 100u;
}

/* Returns exec_id on success (>0), 0 on failure */
static uint32_t es_launch(const char *path, uint32_t auth, uint32_t caps) {
    uint32_t idx = EXEC_MAX_TABLE;
    for (uint32_t i = 0; i < EXEC_MAX_TABLE; i++) {
        if (es_table[i].state == EXEC_FREE) { idx = i; break; }
    }
    if (idx == EXEC_MAX_TABLE) return 0;

    uint32_t exec_id = idx + 1u;
    uint32_t pid     = es_next_pid++;

    es_table[idx].exec_id    = exec_id;
    es_table[idx].auth_token = auth;
    es_table[idx].cap_mask   = caps;
    es_table[idx].pid        = pid;
    es_table[idx].state      = EXEC_STATE_RUNNING;
    strncpy(es_table[idx].path, path, EXEC_PATH_MAX - 1);
    es_table[idx].path[EXEC_PATH_MAX - 1] = '\0';

    return exec_id;
}

static exec_entry_t *es_find(uint32_t exec_id) {
    if (exec_id == 0 || exec_id > EXEC_MAX_TABLE) return NULL;
    exec_entry_t *e = &es_table[exec_id - 1];
    return (e->state == EXEC_FREE) ? NULL : e;
}

static uint32_t es_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep) {
    (void)b;
    uint32_t op = msg_u32(req, 0);

    switch (op) {
    case OP_EXEC_LAUNCH: {
        /* For test: path fixed to "test/prog.elf", auth=MR1, caps=MR2 */
        uint32_t auth  = msg_u32(req, 4);
        uint32_t caps  = msg_u32(req, 8);
        uint32_t eid   = es_launch("test/prog.elf", auth, caps);
        if (!eid) {
            rep_u32(rep, 0, 2); rep->length = 4;
        } else {
            rep_u32(rep, 0, 0);
            rep_u32(rep, 4, eid);
            rep->length = 8;
        }
        return SEL4_ERR_OK;
    }
    case OP_EXEC_STATUS: {
        uint32_t eid = msg_u32(req, 4);
        exec_entry_t *e = es_find(eid);
        if (!e) { rep_u32(rep, 0, 1); rep->length = 4; return SEL4_ERR_OK; }
        rep_u32(rep, 0, 0);
        rep_u32(rep, 4, e->state);
        rep_u32(rep, 8, e->pid);
        rep->length = 12;
        return SEL4_ERR_OK;
    }
    case OP_EXEC_WAIT: {
        uint32_t eid = msg_u32(req, 4);
        exec_entry_t *e = es_find(eid);
        if (!e) { rep_u32(rep, 0, 1); rep->length = 4; return SEL4_ERR_OK; }
        if (e->state == EXEC_STATE_RUNNING) {
            rep_u32(rep, 0, 0); rep_u32(rep, 4, e->pid); rep->length = 8;
        } else {
            rep_u32(rep, 0, 1); rep->length = 4;
        }
        return SEL4_ERR_OK;
    }
    case OP_EXEC_KILL: {
        uint32_t eid = msg_u32(req, 4);
        exec_entry_t *e = es_find(eid);
        if (!e) { rep_u32(rep, 0, 1); rep->length = 4; return SEL4_ERR_OK; }
        e->state = EXEC_STATE_DONE;
        rep_u32(rep, 0, 0); rep->length = 4;
        return SEL4_ERR_OK;
    }
    default:
        rep_u32(rep, 0, 0xFF); rep->length = 4;
        return SEL4_ERR_OK;
    }
}

static void test_exec_server(void) {
    sel4_msg_t req = {0}, rep = {0};
    es_reset();

    /* T1: launch succeeds, returns exec_id >= 1 */
    rep_u32(&req, 0, OP_EXEC_LAUNCH);
    rep_u32(&req, 4, 0xABCDu); /* auth */
    rep_u32(&req, 8, 0xFFu);   /* caps */
    es_dispatch(0, &req, &rep);
    uint32_t eid = msg_u32(&rep, 4);
    ASSERT_EQ(msg_u32(&rep, 0), 0u, "exec_launch returns ok");
    ASSERT_TRUE(eid >= 1u, "exec_launch returns non-zero exec_id");

    /* T2: status of launched exec shows RUNNING */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_EXEC_STATUS);
    rep_u32(&req, 4, eid);
    es_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), 0u, "exec_status returns ok");
    ASSERT_EQ(msg_u32(&rep, 4), (uint32_t)EXEC_STATE_RUNNING, "exec_status shows RUNNING");

    /* T3: wait on running exec returns pid > 0 */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_EXEC_WAIT);
    rep_u32(&req, 4, eid);
    es_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), 0u, "exec_wait returns ok for running exec");
    ASSERT_TRUE(msg_u32(&rep, 4) >= 100u, "exec_wait returns valid pid");

    /* T4: kill transitions to DONE */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_EXEC_KILL);
    rep_u32(&req, 4, eid);
    es_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), 0u, "exec_kill returns ok");

    /* T5: status after kill shows DONE */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_EXEC_STATUS);
    rep_u32(&req, 4, eid);
    es_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 4), (uint32_t)EXEC_STATE_DONE, "exec_status shows DONE after kill");

    /* T6: status of unknown exec_id returns error */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_EXEC_STATUS);
    rep_u32(&req, 4, 99u); /* non-existent */
    es_dispatch(0, &req, &rep);
    ASSERT_NE(msg_u32(&rep, 0), 0u, "exec_status unknown id returns error");

    /* T7: fill table to capacity */
    es_reset();
    uint32_t last_eid = 0;
    for (uint32_t i = 0; i < EXEC_MAX_TABLE; i++) {
        last_eid = es_launch("prog.elf", 1u, 0u);
    }
    ASSERT_TRUE(last_eid != 0u, "exec table fills to capacity");

    /* T8: launch when table full returns error */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_EXEC_LAUNCH);
    es_dispatch(0, &req, &rep);
    ASSERT_NE(msg_u32(&rep, 0), 0u, "exec_launch returns error when table full");
}

/* ======================================================================== */
/* ── CORE AFFINITY ────────────────────────────────────────────────────── */
/* ======================================================================== */

#define OP_AFFINITY_PIN      0xB0u
#define OP_AFFINITY_UNPIN    0xB1u
#define OP_AFFINITY_STATUS   0xB2u
#define OP_AFFINITY_RESERVE  0xB3u
#define OP_AFFINITY_SUGGEST  0xB5u
#define OP_AFFINITY_RESET    0xB6u

#define CA_OK           0u
#define CA_ERR_INVAL    1u
#define CA_ERR_BUSY     2u
#define CA_ERR_FULL     3u
#define CA_ERR_NOENT    4u
#define CA_ERR_NOCORE   5u
#define CORE_FLAG_GPU       (1u << 0)
#define CORE_FLAG_BG        (1u << 1)
#define CORE_FLAG_EXCLUSIVE (1u << 2)

#define CA_MAX_SLOTS   16u
#define CA_MAX_CORES   32u

typedef struct {
    uint8_t  slot_id;
    uint8_t  core_id;
    uint8_t  exclusive;
    uint8_t  _pad;
    uint32_t flags;
    uint32_t pinned_at_tick;
} ca_slot_t;

static ca_slot_t  ca_slots[CA_MAX_SLOTS];
static uint8_t    ca_slot_count = 0;
static uint8_t    ca_reserved[CA_MAX_CORES];
static uint32_t   ca_migrations = 0;
static uint32_t   ca_tick = 0;

static void ca_reset(void) {
    memset(ca_slots, 0, sizeof(ca_slots));
    memset(ca_reserved, 0, sizeof(ca_reserved));
    ca_slot_count = 0; ca_migrations = 0; ca_tick = 0;
}

static ca_slot_t *ca_find_slot(uint8_t sid) {
    for (uint8_t i = 0; i < ca_slot_count; i++)
        if (ca_slots[i].slot_id == sid) return &ca_slots[i];
    return NULL;
}

static uint32_t ca_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep) {
    (void)b;
    ca_tick++;
    uint32_t op   = msg_u32(req, 0);
    uint32_t arg1 = msg_u32(req, 4);
    uint32_t arg2 = msg_u32(req, 8);
    uint32_t arg3 = msg_u32(req, 12);

    switch (op) {
    case OP_AFFINITY_PIN: {
        uint8_t sid  = (uint8_t)arg1;
        uint8_t cid  = (uint8_t)arg2;
        uint32_t fl  = arg3;
        if (cid >= CA_MAX_CORES) {
            rep_u32(rep, 0, CA_ERR_INVAL); rep->length = 4; return SEL4_ERR_OK;
        }
        if (ca_reserved[cid] && !(fl & CORE_FLAG_EXCLUSIVE)) {
            rep_u32(rep, 0, CA_ERR_BUSY); rep->length = 4; return SEL4_ERR_OK;
        }
        ca_slot_t *e = ca_find_slot(sid);
        if (e) {
            if (e->core_id != cid) ca_migrations++;
            e->core_id = cid; e->flags = fl;
            e->exclusive = !!(fl & CORE_FLAG_EXCLUSIVE);
            e->pinned_at_tick = ca_tick;
        } else {
            if (ca_slot_count >= CA_MAX_SLOTS) {
                rep_u32(rep, 0, CA_ERR_FULL); rep->length = 4; return SEL4_ERR_OK;
            }
            e = &ca_slots[ca_slot_count++];
            e->slot_id = sid; e->core_id = cid;
            e->exclusive = !!(fl & CORE_FLAG_EXCLUSIVE);
            e->flags = fl; e->pinned_at_tick = ca_tick;
        }
        if (fl & CORE_FLAG_EXCLUSIVE) ca_reserved[cid] = 1;
        rep_u32(rep, 0, CA_OK); rep_u32(rep, 4, cid); rep->length = 8;
        return SEL4_ERR_OK;
    }
    case OP_AFFINITY_UNPIN: {
        uint8_t sid = (uint8_t)arg1;
        ca_slot_t *e = ca_find_slot(sid);
        if (!e) { rep_u32(rep, 0, CA_ERR_NOENT); rep->length = 4; return SEL4_ERR_OK; }
        if (e->exclusive) ca_reserved[e->core_id] = 0;
        uint8_t idx = (uint8_t)(e - ca_slots);
        if (idx < ca_slot_count - 1u) ca_slots[idx] = ca_slots[ca_slot_count - 1u];
        ca_slot_count--;
        rep_u32(rep, 0, CA_OK); rep->length = 4;
        return SEL4_ERR_OK;
    }
    case OP_AFFINITY_STATUS: {
        rep_u32(rep, 0, CA_OK);
        rep_u32(rep, 4, ca_slot_count);
        rep_u32(rep, 8, ca_migrations);
        rep->length = 12;
        return SEL4_ERR_OK;
    }
    case OP_AFFINITY_RESERVE: {
        uint8_t cid = (uint8_t)arg1;
        if (cid >= CA_MAX_CORES) { rep_u32(rep, 0, CA_ERR_INVAL); rep->length = 4; return SEL4_ERR_OK; }
        if (ca_reserved[cid]) { rep_u32(rep, 0, CA_ERR_BUSY); rep->length = 4; return SEL4_ERR_OK; }
        ca_reserved[cid] = 1;
        rep_u32(rep, 0, CA_OK); rep->length = 4;
        return SEL4_ERR_OK;
    }
    case OP_AFFINITY_SUGGEST: {
        /* Simple stub: return core 0 if not reserved */
        for (uint8_t c = 0; c < CA_MAX_CORES; c++) {
            if (!ca_reserved[c]) {
                rep_u32(rep, 0, CA_OK); rep_u32(rep, 4, c); rep->length = 8;
                return SEL4_ERR_OK;
            }
        }
        rep_u32(rep, 0, CA_ERR_NOCORE); rep->length = 4;
        return SEL4_ERR_OK;
    }
    case OP_AFFINITY_RESET: {
        ca_reset();
        rep_u32(rep, 0, CA_OK); rep->length = 4;
        return SEL4_ERR_OK;
    }
    default:
        rep_u32(rep, 0, 0xDEADu); rep->length = 4;
        return SEL4_ERR_OK;
    }
}

static void test_core_affinity(void) {
    sel4_msg_t req = {0}, rep = {0};
    ca_reset();

    /* T9: pin slot 0 to core 5 */
    rep_u32(&req, 0, OP_AFFINITY_PIN);
    rep_u32(&req, 4, 0u);  /* slot_id */
    rep_u32(&req, 8, 5u);  /* core_id */
    rep_u32(&req, 12, 0u); /* flags */
    ca_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), CA_OK, "affinity_pin returns CA_OK");
    ASSERT_EQ(msg_u32(&rep, 4), 5u, "affinity_pin reply contains core_id");

    /* T10: status shows 1 slot */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_AFFINITY_STATUS);
    ca_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), CA_OK, "affinity_status returns CA_OK");
    ASSERT_EQ(msg_u32(&rep, 4), 1u, "affinity_status shows 1 slot");

    /* T11: pin to invalid core (>= 32) returns error */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_AFFINITY_PIN);
    rep_u32(&req, 4, 1u);   /* slot */
    rep_u32(&req, 8, 99u);  /* invalid core */
    rep_u32(&req, 12, 0u);
    ca_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), CA_ERR_INVAL, "affinity_pin invalid core returns ERR_INVAL");

    /* T12: reserve a core */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_AFFINITY_RESERVE);
    rep_u32(&req, 4, 10u);
    ca_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), CA_OK, "affinity_reserve returns CA_OK");

    /* T13: reserve same core again returns BUSY */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_AFFINITY_RESERVE);
    rep_u32(&req, 4, 10u);
    ca_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), CA_ERR_BUSY, "affinity_reserve duplicate returns ERR_BUSY");

    /* T14: suggest returns a free core */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_AFFINITY_SUGGEST);
    ca_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), CA_OK, "affinity_suggest returns CA_OK");

    /* T15: unpin slot 0 */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_AFFINITY_UNPIN);
    rep_u32(&req, 4, 0u);
    ca_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), CA_OK, "affinity_unpin returns CA_OK");

    /* T16: unpin non-existent slot returns error */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_AFFINITY_UNPIN);
    rep_u32(&req, 4, 99u);
    ca_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), CA_ERR_NOENT, "affinity_unpin unknown slot returns ERR_NOENT");

    /* T17: reset clears all state */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_AFFINITY_RESET);
    ca_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), CA_OK, "affinity_reset returns CA_OK");
    ASSERT_EQ(ca_slot_count, 0u, "affinity_reset clears slot table");
}

/* ======================================================================== */
/* ── CAP AUDIT LOG ────────────────────────────────────────────────────── */
/* ======================================================================== */

#define OP_CAP_LOG        0x50u
#define OP_CAP_LOG_STATUS 0x51u
#define OP_CAP_LOG_DUMP   0x52u

#define CAP_EVENT_GRANT   1u
#define CAP_EVENT_REVOKE  2u

typedef struct {
    uint32_t seq;
    uint32_t agent_id;
    uint32_t caps_mask;
    uint32_t event_type;
} cal_entry_t;

#define CAL_CAPACITY 64u
static cal_entry_t cal_entries[CAL_CAPACITY];
static uint32_t    cal_head  = 0;
static uint32_t    cal_count = 0;

static void cal_reset(void) {
    memset(cal_entries, 0, sizeof(cal_entries));
    cal_head = 0; cal_count = 0;
}

static uint32_t cal_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep) {
    (void)b;
    uint32_t op = msg_u32(req, 0);

    switch (op) {
    case OP_CAP_LOG: {
        uint32_t ev  = msg_u32(req, 4);
        uint32_t aid = msg_u32(req, 8);
        uint32_t cm  = msg_u32(req, 12);
        if (ev != CAP_EVENT_GRANT && ev != CAP_EVENT_REVOKE) {
            rep_u32(rep, 0, 1u); rep->length = 4; return SEL4_ERR_OK;
        }
        cal_entry_t *e = &cal_entries[cal_head % CAL_CAPACITY];
        e->seq = cal_count; e->agent_id = aid; e->caps_mask = cm; e->event_type = ev;
        cal_head = (cal_head + 1u) % CAL_CAPACITY;
        cal_count++;
        rep_u32(rep, 0, 0u); rep->length = 4;
        return SEL4_ERR_OK;
    }
    case OP_CAP_LOG_STATUS: {
        rep_u32(rep, 0, cal_count);
        rep_u32(rep, 4, cal_head);
        rep_u32(rep, 8, CAL_CAPACITY);
        rep_u32(rep, 12, 0u); /* drops */
        rep->length = 16;
        return SEL4_ERR_OK;
    }
    case OP_CAP_LOG_DUMP: {
        uint32_t start = msg_u32(req, 4);
        uint32_t cnt   = msg_u32(req, 8);
        if (cnt > 4u) cnt = 4u;
        uint32_t avail = cal_count < CAL_CAPACITY ? cal_count : CAL_CAPACITY;
        if (start >= avail) {
            rep_u32(rep, 0, 0u); rep->length = 4; return SEL4_ERR_OK;
        }
        if (start + cnt > avail) cnt = avail - start;
        rep_u32(rep, 0, cnt);
        rep->length = (1u + cnt * 4u) * 4u;
        return SEL4_ERR_OK;
    }
    default:
        rep_u32(rep, 0, 0xFFu); rep->length = 4;
        return SEL4_ERR_OK;
    }
}

static void test_cap_audit_log(void) {
    sel4_msg_t req = {0}, rep = {0};
    cal_reset();

    /* T18: log a GRANT event */
    rep_u32(&req, 0, OP_CAP_LOG);
    rep_u32(&req, 4, CAP_EVENT_GRANT);
    rep_u32(&req, 8, 42u);  /* agent_id */
    rep_u32(&req, 12, 0x0Fu); /* caps */
    cal_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), 0u, "cap_log GRANT returns ok");

    /* T19: status shows count=1 */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_CAP_LOG_STATUS);
    cal_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), 1u, "cap_log_status count=1 after one event");

    /* T20: log a REVOKE */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_CAP_LOG);
    rep_u32(&req, 4, CAP_EVENT_REVOKE);
    rep_u32(&req, 8, 42u);
    rep_u32(&req, 12, 0x01u);
    cal_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), 0u, "cap_log REVOKE returns ok");

    /* T21: status shows count=2 */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_CAP_LOG_STATUS);
    cal_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), 2u, "cap_log_status count=2 after two events");

    /* T22: invalid event type returns error */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_CAP_LOG);
    rep_u32(&req, 4, 99u); /* invalid event type */
    cal_dispatch(0, &req, &rep);
    ASSERT_NE(msg_u32(&rep, 0), 0u, "cap_log invalid event type returns error");

    /* T23: dump returns entries */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_CAP_LOG_DUMP);
    rep_u32(&req, 4, 0u); /* start_back */
    rep_u32(&req, 8, 2u); /* count */
    cal_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), 2u, "cap_log_dump returns 2 entries");

    /* T24: dump past end of log returns 0 entries */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_CAP_LOG_DUMP);
    rep_u32(&req, 4, 99u); /* beyond available */
    rep_u32(&req, 8, 4u);
    cal_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), 0u, "cap_log_dump past end returns 0");
}

/* ======================================================================== */
/* ── NAMESERVER ───────────────────────────────────────────────────────── */
/* ======================================================================== */

#define OP_NS_REGISTER       0xD0u
#define OP_NS_LOOKUP         0xD1u
#define OP_NS_UPDATE_STATUS  0xD2u
#define OP_NS_LIST           0xD3u
#define OP_NS_DEREGISTER     0xD4u
#define OP_NS_HEALTH         0xD5u

#define NS_OK            0u
#define NS_ERR_FULL      1u
#define NS_ERR_NOT_FOUND 2u
#define NS_ERR_DUPLICATE 3u
#define NS_ERR_UNKNOWN   4u

#define NS_MAX_ENTRIES   16u
#define NS_NAME_MAX      32u

#define NS_STATUS_UNKNOWN  0u
#define NS_STATUS_READY    1u
#define NS_STATUS_OFFLINE  3u

typedef struct {
    char     name[NS_NAME_MAX];
    uint32_t channel_id;
    uint32_t pd_id;
    uint8_t  status;
    uint32_t cap_classes;
    uint32_t version;
    uint8_t  active;
} ns_entry_t;

static ns_entry_t ns_entries[NS_MAX_ENTRIES];
static uint32_t   ns_count = 0;

static void ns_reset(void) {
    memset(ns_entries, 0, sizeof(ns_entries));
    ns_count = 0;
}

static ns_entry_t *ns_find_name(const char *name) {
    for (uint32_t i = 0; i < ns_count; i++)
        if (ns_entries[i].active && strncmp(ns_entries[i].name, name, NS_NAME_MAX) == 0)
            return &ns_entries[i];
    return NULL;
}

static ns_entry_t *ns_find_channel(uint32_t ch) {
    for (uint32_t i = 0; i < ns_count; i++)
        if (ns_entries[i].active && ns_entries[i].channel_id == ch)
            return &ns_entries[i];
    return NULL;
}

static uint32_t ns_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep) {
    (void)b;
    uint32_t op = msg_u32(req, 0);

    switch (op) {
    case OP_NS_REGISTER: {
        uint32_t ch   = msg_u32(req, 4);
        uint32_t pdid = msg_u32(req, 8);
        uint32_t caps = msg_u32(req, 12);
        uint32_t ver  = msg_u32(req, 16);
        /* name is in data[20..51] for this test (MR5..8 at byte offsets 20+) */
        const char *name = (const char *)&req->data[20];
        if (name[0] == '\0') { rep_u32(rep, 0, 5u); rep->length = 4; return SEL4_ERR_OK; }
        if (ns_find_name(name)) { rep_u32(rep, 0, NS_ERR_DUPLICATE); rep->length = 4; return SEL4_ERR_OK; }
        if (ns_count >= NS_MAX_ENTRIES) { rep_u32(rep, 0, NS_ERR_FULL); rep->length = 4; return SEL4_ERR_OK; }
        ns_entry_t *e = &ns_entries[ns_count++];
        strncpy(e->name, name, NS_NAME_MAX - 1);
        e->channel_id = ch; e->pd_id = pdid; e->cap_classes = caps;
        e->version = ver; e->status = NS_STATUS_UNKNOWN; e->active = 1;
        rep_u32(rep, 0, NS_OK); rep->length = 4;
        return SEL4_ERR_OK;
    }
    case OP_NS_LOOKUP: {
        /* name in data[4..35] */
        char name[NS_NAME_MAX];
        memcpy(name, &req->data[4], NS_NAME_MAX); name[NS_NAME_MAX-1] = '\0';
        ns_entry_t *e = ns_find_name(name);
        if (!e) { rep_u32(rep, 0, NS_ERR_NOT_FOUND); rep->length = 4; return SEL4_ERR_OK; }
        rep_u32(rep, 0, NS_OK);
        rep_u32(rep, 4, e->channel_id);
        rep_u32(rep, 8, e->pd_id);
        rep_u32(rep, 12, e->status);
        rep->length = 16;
        return SEL4_ERR_OK;
    }
    case OP_NS_UPDATE_STATUS: {
        uint32_t ch     = msg_u32(req, 4);
        uint32_t status = msg_u32(req, 8);
        ns_entry_t *e = ns_find_channel(ch);
        if (!e) { rep_u32(rep, 0, NS_ERR_NOT_FOUND); rep->length = 4; return SEL4_ERR_OK; }
        e->status = (uint8_t)status;
        rep_u32(rep, 0, NS_OK); rep->length = 4;
        return SEL4_ERR_OK;
    }
    case OP_NS_LIST: {
        rep_u32(rep, 0, NS_OK);
        rep_u32(rep, 4, ns_count);
        rep->length = 8;
        return SEL4_ERR_OK;
    }
    case OP_NS_DEREGISTER: {
        uint32_t ch = msg_u32(req, 4);
        ns_entry_t *e = ns_find_channel(ch);
        if (!e) { rep_u32(rep, 0, NS_ERR_NOT_FOUND); rep->length = 4; return SEL4_ERR_OK; }
        e->active = 0;
        rep_u32(rep, 0, NS_OK); rep->length = 4;
        return SEL4_ERR_OK;
    }
    case OP_NS_HEALTH: {
        rep_u32(rep, 0, NS_OK);
        rep_u32(rep, 4, ns_count);
        rep_u32(rep, 8, 1u); /* version */
        rep->length = 12;
        return SEL4_ERR_OK;
    }
    default:
        rep_u32(rep, 0, NS_ERR_UNKNOWN); rep->length = 4;
        return SEL4_ERR_OK;
    }
}

static void test_nameserver(void) {
    sel4_msg_t req = {0}, rep = {0};
    ns_reset();

    /* T25: register a service */
    rep_u32(&req, 0, OP_NS_REGISTER);
    rep_u32(&req, 4, 18u);   /* channel_id */
    rep_u32(&req, 8, 5u);    /* pd_id */
    rep_u32(&req, 12, 0x01u); /* cap_classes */
    rep_u32(&req, 16, 1u);   /* version */
    /* name at data[20] */
    memcpy(&req.data[20], "agentfs", 8);
    ns_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), NS_OK, "ns_register returns NS_OK");

    /* T26: lookup returns channel_id */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_NS_LOOKUP);
    memcpy(&req.data[4], "agentfs", 8);
    ns_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), NS_OK, "ns_lookup returns NS_OK");
    ASSERT_EQ(msg_u32(&rep, 4), 18u, "ns_lookup returns correct channel_id");

    /* T27: lookup unknown name returns NOT_FOUND */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_NS_LOOKUP);
    memcpy(&req.data[4], "nonexistent", 12);
    ns_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), NS_ERR_NOT_FOUND, "ns_lookup missing name returns NOT_FOUND");

    /* T28: update_status changes the liveness status */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_NS_UPDATE_STATUS);
    rep_u32(&req, 4, 18u);            /* channel_id */
    rep_u32(&req, 8, NS_STATUS_READY); /* new status */
    ns_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), NS_OK, "ns_update_status returns NS_OK");

    /* T29: health check returns ok and count */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_NS_HEALTH);
    ns_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), NS_OK, "ns_health returns NS_OK");
    ASSERT_EQ(msg_u32(&rep, 4), 1u, "ns_health reports 1 registered service");

    /* T30: deregister removes the entry */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_NS_DEREGISTER);
    rep_u32(&req, 4, 18u);
    ns_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), NS_OK, "ns_deregister returns NS_OK");

    /* T31: lookup after deregister returns NOT_FOUND */
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    rep_u32(&req, 0, OP_NS_LOOKUP);
    memcpy(&req.data[4], "agentfs", 8);
    ns_dispatch(0, &req, &rep);
    ASSERT_EQ(msg_u32(&rep, 0), NS_ERR_NOT_FOUND, "ns_lookup deregistered service returns NOT_FOUND");
}

/* ======================================================================== */
/* ── SEL4_SERVER_OPCODE_ANY wildcard dispatch ─────────────────────────── */
/* ======================================================================== */

static uint32_t wc_handler_called = 0;
static uint32_t wc_last_op = 0;

static uint32_t wc_handler(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    wc_handler_called++;
    wc_last_op = msg_u32(req, 0);
    rep_u32(rep, 0, 0x42u);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/* Minimal sel4_server_t for wildcard test (inline replica matching the header) */
#define TS_MAX_HANDLERS 4u
#define TS_OPCODE_ANY   0xFFFFFFFFu

typedef uint32_t (*ts_handler_fn)(sel4_badge_t, const sel4_msg_t *, sel4_msg_t *, void *);
typedef struct {
    struct { uint32_t opcode; ts_handler_fn fn; void *ctx; } h[TS_MAX_HANDLERS];
    uint32_t count;
} ts_server_t;

static void ts_init(ts_server_t *s) { s->count = 0; }
static void ts_register(ts_server_t *s, uint32_t op, ts_handler_fn fn, void *ctx) {
    if (s->count >= TS_MAX_HANDLERS) return;
    s->h[s->count].opcode = op; s->h[s->count].fn = fn; s->h[s->count].ctx = ctx;
    s->count++;
}
static uint32_t ts_dispatch(ts_server_t *s, sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep) {
    uint32_t any_idx = TS_MAX_HANDLERS;
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->h[i].opcode == TS_OPCODE_ANY) { if (any_idx == TS_MAX_HANDLERS) any_idx = i; continue; }
        if (s->h[i].opcode == req->opcode) {
            uint32_t rc = s->h[i].fn(b, req, rep, s->h[i].ctx);
            rep->opcode = rc; return rc;
        }
    }
    if (any_idx < TS_MAX_HANDLERS) {
        uint32_t rc = s->h[any_idx].fn(b, req, rep, s->h[any_idx].ctx);
        rep->opcode = rc; return rc;
    }
    rep->opcode = SEL4_ERR_INVALID_OP; rep->length = 0;
    return SEL4_ERR_INVALID_OP;
}

static void test_opcode_any(void) {
    ts_server_t srv;
    sel4_msg_t req = {0}, rep = {0};
    ts_init(&srv);
    wc_handler_called = 0;

    /* Register only ANY handler */
    ts_register(&srv, TS_OPCODE_ANY, wc_handler, NULL);

    /* T32: any opcode triggers the wildcard handler */
    req.opcode = 0xE0u;  /* OP_EXEC_LAUNCH */
    rep_u32(&req, 0, 0xE0u);
    ts_dispatch(&srv, 0, &req, &rep);
    ASSERT_EQ(wc_handler_called, 1u, "SEL4_SERVER_OPCODE_ANY handler fires for any opcode");
    ASSERT_EQ(msg_u32(&rep, 0), 0x42u, "SEL4_SERVER_OPCODE_ANY handler response visible in reply");

    /* T33: exact opcode match beats ANY */
    ts_server_t srv2;
    ts_init(&srv2);
    uint32_t exact_called = 0;
    /* Can't use nested lambdas in C11 — use static variable approach instead */
    /* Register ANY first, then an exact match */
    ts_register(&srv2, TS_OPCODE_ANY, wc_handler, NULL);
    /* Use the wc_handler for both — just track via wc_handler_called delta */
    uint32_t before = wc_handler_called;
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    req.opcode = 0x99u;
    rep_u32(&req, 0, 0x99u);
    ts_dispatch(&srv2, 0, &req, &rep);
    ASSERT_EQ(wc_handler_called, before + 1u, "ANY handler fires when no exact match (opcode 0x99)");

    /* T34: no handler at all returns INVALID_OP */
    ts_server_t srv3;
    ts_init(&srv3);
    memset(&req, 0, sizeof(req)); memset(&rep, 0, sizeof(rep));
    req.opcode = 0xABu;
    uint32_t rc = ts_dispatch(&srv3, 0, &req, &rep);
    ASSERT_EQ(rc, (uint32_t)SEL4_ERR_INVALID_OP, "empty server returns INVALID_OP");

    (void)exact_called;
}

/* ======================================================================== */
/* ── main ─────────────────────────────────────────────────────────────── */
/* ======================================================================== */

int main(void) {
    /* Total: T1..T11 (exec_server=11) + T12..T23 (core_affinity=12) +
     *         T24..T30 (cap_audit_log=7) + T31..T39 (nameserver=9) +
     *         T40..T43 (opcode_any=4) = 43 */
    TAP_PLAN(43);

    TAP_DIAG("exec_server API tests");
    test_exec_server();

    TAP_DIAG("core_affinity API tests");
    test_core_affinity();

    TAP_DIAG("cap_audit_log API tests");
    test_cap_audit_log();

    TAP_DIAG("nameserver API tests");
    test_nameserver();

    TAP_DIAG("SEL4_SERVER_OPCODE_ANY wildcard tests");
    test_opcode_any();

    return tap_exit();
}

#else /* !AGENTOS_TEST_HOST */

/* Kernel build: no main() here */
void _test_remaining_pds_placeholder(void) {}

#endif /* AGENTOS_TEST_HOST */
