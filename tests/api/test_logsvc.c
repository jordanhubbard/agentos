/*
 * test_logsvc.c — API tests for the agentOS LogSvc (log service)
 *
 * Covered opcodes:
 *   OP_LOG_WRITE   (0x40) — write a log entry (level, message)
 *   OP_LOG_QUERY   (0x41) — retrieve recent entries (level filter, max count)
 *   OP_LOG_FLUSH   (0x42) — flush / clear the in-memory log ring
 *   unknown opcode         — must return AOS_ERR_UNIMPL
 *
 * TODO: replace inline mock with
 *       #include "../../contracts/logsvc/interface.h"
 *
 * Build & run:
 *   cc -DAGENTOS_TEST_HOST -I tests/api -o /tmp/t_logsvc \
 *       tests/api/test_logsvc.c && /tmp/t_logsvc
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST
#include "framework.h"

/* ── Opcodes ─────────────────────────────────────────────────────────────── */
/* TODO: replace with #include "../../contracts/logsvc/interface.h" */
#define OP_LOG_WRITE   0x40u
#define OP_LOG_QUERY   0x41u
#define OP_LOG_FLUSH   0x42u

/* ── Log levels ───────────────────────────────────────────────────────────── */
#define LOG_LEVEL_DEBUG   0u
#define LOG_LEVEL_INFO    1u
#define LOG_LEVEL_WARN    2u
#define LOG_LEVEL_ERROR   3u
#define LOG_LEVEL_FATAL   4u

/* ── Mock LogSvc ──────────────────────────────────────────────────────────── */

#define MOCK_LOG_MAX_ENTRIES 64u
#define MOCK_LOG_MAX_MSG     256u

typedef struct {
    uint32_t level;
    char     message[MOCK_LOG_MAX_MSG];
    uint32_t msg_len;
    uint64_t timestamp_us;
    uint32_t used;
} MockLogEntry;

static MockLogEntry g_log[MOCK_LOG_MAX_ENTRIES];
static uint32_t     g_log_head  = 0u;
static uint32_t     g_log_count = 0u;
static uint64_t     g_clock_us  = 0u;

static void logsvc_reset(void) {
    memset(g_log, 0, sizeof(g_log));
    g_log_head  = 0u;
    g_log_count = 0u;
    g_clock_us  = 1000u;
}

/*
 * log_dispatch — mock LogSvc IPC handler.
 *
 * MR layout:
 *   OP_LOG_WRITE:
 *     MR[0] = OP_LOG_WRITE
 *     MR[1] = level (0=debug .. 4=fatal)
 *     MR[2] = message pointer (host ptr in tests)
 *     MR[3] = message length
 *     Reply: MR[0] = AOS_OK | AOS_ERR_INVAL | AOS_ERR_TOO_LARGE
 *
 *   OP_LOG_QUERY:
 *     MR[0] = OP_LOG_QUERY
 *     MR[1] = minimum level (entries below this level are skipped)
 *     MR[2] = max_count (0 = return all matching)
 *     Reply: MR[0] = AOS_OK, MR[1] = count of matching entries available
 *
 *   OP_LOG_FLUSH:
 *     MR[0] = OP_LOG_FLUSH
 *     Reply: MR[0] = AOS_OK, MR[1] = entries discarded
 */
static void log_dispatch(microkit_channel ch, microkit_msginfo info) {
    (void)ch; (void)info;
    uint64_t op = _mrs[0];

    switch (op) {

    /* ── WRITE ──────────────────────────────────────────────────────────── */
    case OP_LOG_WRITE: {
        uint32_t    level = (uint32_t)_mrs[1];
        const char *msg   = (const char *)(uintptr_t)_mrs[2];
        uint32_t    mlen  = (uint32_t)_mrs[3];

        if (level > LOG_LEVEL_FATAL) {
            _mrs[0] = AOS_ERR_INVAL; break;
        }
        if (!msg || mlen == 0) {
            _mrs[0] = AOS_ERR_INVAL; break;
        }
        if (mlen >= MOCK_LOG_MAX_MSG) {
            _mrs[0] = AOS_ERR_TOO_LARGE; break;
        }
        /* Ring-buffer insertion */
        uint32_t slot = g_log_head % MOCK_LOG_MAX_ENTRIES;
        MockLogEntry *e = &g_log[slot];
        e->level        = level;
        e->msg_len      = mlen;
        e->timestamp_us = g_clock_us++;
        e->used         = 1u;
        memcpy(e->message, msg, mlen);
        e->message[mlen] = '\0';
        g_log_head++;
        if (g_log_count < MOCK_LOG_MAX_ENTRIES) g_log_count++;
        _mrs[0] = AOS_OK;
        break;
    }

    /* ── QUERY ──────────────────────────────────────────────────────────── */
    case OP_LOG_QUERY: {
        uint32_t min_level = (uint32_t)_mrs[1];
        uint32_t max_count = (uint32_t)_mrs[2];

        if (max_count == 0) max_count = g_log_count;

        uint32_t matched = 0u;
        for (uint32_t i = 0; i < MOCK_LOG_MAX_ENTRIES && matched < max_count; i++) {
            if (g_log[i].used && g_log[i].level >= min_level) matched++;
        }
        _mrs[0] = AOS_OK;
        _mrs[1] = (uint64_t)matched;
        break;
    }

    /* ── FLUSH ──────────────────────────────────────────────────────────── */
    case OP_LOG_FLUSH: {
        uint32_t discarded = g_log_count;
        memset(g_log, 0, sizeof(g_log));
        g_log_head  = 0u;
        g_log_count = 0u;
        _mrs[0] = AOS_OK;
        _mrs[1] = (uint64_t)discarded;
        break;
    }

    default:
        _mrs[0] = AOS_ERR_UNIMPL;
        break;
    }
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static uint64_t do_log_write(uint32_t level, const char *msg) {
    mock_mr_clear();
    _mrs[0] = OP_LOG_WRITE;
    _mrs[1] = (uint64_t)level;
    _mrs[2] = (uint64_t)(uintptr_t)msg;
    _mrs[3] = (uint64_t)strlen(msg);
    log_dispatch(0, 0);
    return _mrs[0];
}

static uint32_t do_log_query(uint32_t min_level, uint32_t max_count) {
    mock_mr_clear();
    _mrs[0] = OP_LOG_QUERY;
    _mrs[1] = (uint64_t)min_level;
    _mrs[2] = (uint64_t)max_count;
    log_dispatch(0, 0);
    return (uint32_t)_mrs[1];
}

/* ════════════════════════════════════════════════════════════════════════════
 * Tests
 * ════════════════════════════════════════════════════════════════════════════ */

static void test_write_debug(void) {
    logsvc_reset();
    uint64_t rc = do_log_write(LOG_LEVEL_DEBUG, "debug message");
    ASSERT_EQ(rc, AOS_OK, "LOG_WRITE: debug level returns AOS_OK");
}

static void test_write_info(void) {
    logsvc_reset();
    uint64_t rc = do_log_write(LOG_LEVEL_INFO, "system started");
    ASSERT_EQ(rc, AOS_OK, "LOG_WRITE: info level returns AOS_OK");
}

static void test_write_warn(void) {
    logsvc_reset();
    uint64_t rc = do_log_write(LOG_LEVEL_WARN, "low memory");
    ASSERT_EQ(rc, AOS_OK, "LOG_WRITE: warn level returns AOS_OK");
}

static void test_write_error(void) {
    logsvc_reset();
    uint64_t rc = do_log_write(LOG_LEVEL_ERROR, "ipc failure");
    ASSERT_EQ(rc, AOS_OK, "LOG_WRITE: error level returns AOS_OK");
}

static void test_write_fatal(void) {
    logsvc_reset();
    uint64_t rc = do_log_write(LOG_LEVEL_FATAL, "kernel panic");
    ASSERT_EQ(rc, AOS_OK, "LOG_WRITE: fatal level returns AOS_OK");
}

static void test_write_invalid_level(void) {
    logsvc_reset();
    uint64_t rc = do_log_write(99u, "bad level");
    ASSERT_EQ(rc, AOS_ERR_INVAL, "LOG_WRITE: out-of-range level returns AOS_ERR_INVAL");
}

static void test_write_null_message(void) {
    logsvc_reset();
    mock_mr_clear();
    _mrs[0] = OP_LOG_WRITE;
    _mrs[1] = LOG_LEVEL_INFO;
    _mrs[2] = 0;   /* NULL ptr */
    _mrs[3] = 5;
    log_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_INVAL, "LOG_WRITE: null message ptr returns AOS_ERR_INVAL");
}

static void test_write_empty_message(void) {
    logsvc_reset();
    mock_mr_clear();
    _mrs[0] = OP_LOG_WRITE;
    _mrs[1] = LOG_LEVEL_INFO;
    _mrs[2] = (uint64_t)(uintptr_t)"";
    _mrs[3] = 0;   /* zero length */
    log_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_INVAL, "LOG_WRITE: empty message returns AOS_ERR_INVAL");
}

static void test_write_oversized_message(void) {
    logsvc_reset();
    static char huge[MOCK_LOG_MAX_MSG + 8];
    memset(huge, 'X', sizeof(huge));
    mock_mr_clear();
    _mrs[0] = OP_LOG_WRITE;
    _mrs[1] = LOG_LEVEL_DEBUG;
    _mrs[2] = (uint64_t)(uintptr_t)huge;
    _mrs[3] = sizeof(huge);
    log_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_TOO_LARGE, "LOG_WRITE: oversized message returns AOS_ERR_TOO_LARGE");
}

static void test_query_empty_log(void) {
    logsvc_reset();
    uint32_t count = do_log_query(LOG_LEVEL_DEBUG, 0);
    ASSERT_EQ(count, 0u, "LOG_QUERY: empty log returns count == 0");
    ASSERT_EQ(_mrs[0], AOS_OK, "LOG_QUERY: status AOS_OK on empty log");
}

static void test_query_all_levels(void) {
    logsvc_reset();
    do_log_write(LOG_LEVEL_DEBUG, "d");
    do_log_write(LOG_LEVEL_INFO,  "i");
    do_log_write(LOG_LEVEL_WARN,  "w");
    uint32_t count = do_log_query(LOG_LEVEL_DEBUG, 0);
    ASSERT_EQ(count, 3u, "LOG_QUERY: three entries visible at min=DEBUG");
}

static void test_query_level_filter(void) {
    logsvc_reset();
    do_log_write(LOG_LEVEL_DEBUG, "d1");
    do_log_write(LOG_LEVEL_DEBUG, "d2");
    do_log_write(LOG_LEVEL_WARN,  "w1");
    do_log_write(LOG_LEVEL_ERROR, "e1");
    uint32_t warn_up = do_log_query(LOG_LEVEL_WARN, 0);
    ASSERT_EQ(warn_up, 2u, "LOG_QUERY: level filter WARN+ returns 2 entries");
}

static void test_query_max_count(void) {
    logsvc_reset();
    for (uint32_t i = 0; i < 10; i++) {
        do_log_write(LOG_LEVEL_INFO, "msg");
    }
    uint32_t count = do_log_query(LOG_LEVEL_DEBUG, 5);
    ASSERT_EQ(count, 5u, "LOG_QUERY: max_count=5 caps result at 5");
}

static void test_query_returns_ok(void) {
    logsvc_reset();
    do_log_write(LOG_LEVEL_INFO, "test");
    mock_mr_clear();
    _mrs[0] = OP_LOG_QUERY;
    _mrs[1] = LOG_LEVEL_DEBUG;
    _mrs[2] = 0;
    log_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "LOG_QUERY: status is AOS_OK");
}

static void test_flush_ok(void) {
    logsvc_reset();
    do_log_write(LOG_LEVEL_INFO, "a");
    do_log_write(LOG_LEVEL_WARN, "b");

    mock_mr_clear();
    _mrs[0] = OP_LOG_FLUSH;
    log_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "LOG_FLUSH: returns AOS_OK");
    ASSERT_EQ(_mrs[1], 2u, "LOG_FLUSH: discarded count == 2");
}

static void test_flush_clears_log(void) {
    logsvc_reset();
    do_log_write(LOG_LEVEL_ERROR, "err");
    mock_mr_clear(); _mrs[0] = OP_LOG_FLUSH;
    log_dispatch(0, 0);

    uint32_t count = do_log_query(LOG_LEVEL_DEBUG, 0);
    ASSERT_EQ(count, 0u, "LOG_FLUSH: query after flush returns 0 entries");
}

static void test_flush_empty_log(void) {
    logsvc_reset();
    mock_mr_clear();
    _mrs[0] = OP_LOG_FLUSH;
    log_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "LOG_FLUSH: flush on empty log returns AOS_OK");
    ASSERT_EQ(_mrs[1], 0u, "LOG_FLUSH: discarded == 0 when log was empty");
}

static void test_flush_then_write(void) {
    logsvc_reset();
    do_log_write(LOG_LEVEL_INFO, "before flush");
    mock_mr_clear(); _mrs[0] = OP_LOG_FLUSH; log_dispatch(0, 0);

    uint64_t rc = do_log_write(LOG_LEVEL_INFO, "after flush");
    ASSERT_EQ(rc, AOS_OK, "LOG_FLUSH: write after flush still succeeds");

    uint32_t count = do_log_query(LOG_LEVEL_DEBUG, 0);
    ASSERT_EQ(count, 1u, "LOG_FLUSH: only post-flush entries in query");
}

static void test_ring_buffer_wraps(void) {
    logsvc_reset();
    /* Write more than the ring capacity */
    for (uint32_t i = 0; i < MOCK_LOG_MAX_ENTRIES + 4; i++) {
        do_log_write(LOG_LEVEL_DEBUG, "fill");
    }
    /* Should not crash; count saturates at ring size */
    uint32_t count = do_log_query(LOG_LEVEL_DEBUG, 0);
    ASSERT_TRUE(count <= MOCK_LOG_MAX_ENTRIES,
                "LOG_WRITE/QUERY: ring wrap does not exceed capacity");
}

static void test_unknown_opcode(void) {
    logsvc_reset();
    mock_mr_clear();
    _mrs[0] = 0xCCu;
    log_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_UNIMPL, "unknown opcode returns AOS_ERR_UNIMPL");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    TAP_PLAN(28);

    test_write_debug();
    test_write_info();
    test_write_warn();
    test_write_error();
    test_write_fatal();
    test_write_invalid_level();
    test_write_null_message();
    test_write_empty_message();
    test_write_oversized_message();

    test_query_empty_log();
    test_query_all_levels();
    test_query_level_filter();
    test_query_max_count();
    test_query_returns_ok();

    test_flush_ok();
    test_flush_clears_log();
    test_flush_empty_log();
    test_flush_then_write();

    test_ring_buffer_wraps();

    test_unknown_opcode();

    /* Extra: write all 5 levels then query at ERROR+ */
    {
        logsvc_reset();
        do_log_write(LOG_LEVEL_DEBUG, "d");
        do_log_write(LOG_LEVEL_INFO,  "i");
        do_log_write(LOG_LEVEL_WARN,  "w");
        do_log_write(LOG_LEVEL_ERROR, "e");
        do_log_write(LOG_LEVEL_FATAL, "f");
        uint32_t cnt = do_log_query(LOG_LEVEL_ERROR, 0);
        ASSERT_EQ(cnt, 2u, "LOG_QUERY: ERROR+ filter returns 2 of 5 entries");
    }

    /* Extra: timestamps are monotonically increasing */
    {
        logsvc_reset();
        do_log_write(LOG_LEVEL_INFO, "first");
        do_log_write(LOG_LEVEL_INFO, "second");
        ASSERT_TRUE(g_log[1].timestamp_us > g_log[0].timestamp_us,
                    "LOG_WRITE: timestamps increase between consecutive entries");
    }

    /* Extra: flush discarded count equals what was written */
    {
        logsvc_reset();
        do_log_write(LOG_LEVEL_WARN, "x");
        do_log_write(LOG_LEVEL_WARN, "y");
        do_log_write(LOG_LEVEL_WARN, "z");
        mock_mr_clear(); _mrs[0] = OP_LOG_FLUSH; log_dispatch(0, 0);
        ASSERT_EQ(_mrs[1], 3u, "LOG_FLUSH: discarded count matches write count");
    }

    /* Extra: write survives max-1 ring capacity without error */
    {
        logsvc_reset();
        bool all_ok = true;
        for (uint32_t i = 0; i < MOCK_LOG_MAX_ENTRIES; i++) {
            uint64_t rc = do_log_write(LOG_LEVEL_DEBUG, "capacity test");
            if (rc != AOS_OK) { all_ok = false; break; }
        }
        ASSERT_TRUE(all_ok, "LOG_WRITE: all writes up to ring capacity succeed");
    }

    return tap_exit();
}

#else
typedef int _agentos_api_test_logsvc_dummy;
#endif /* AGENTOS_TEST_HOST */
