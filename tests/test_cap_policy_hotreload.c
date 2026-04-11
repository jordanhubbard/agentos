/*
 * agentOS Cap-Policy Hot-Reload — Unit Test
 *
 * Tests the CBOR policy parser and runtime policy enforcement logic by
 * exercising the parse_policy_blob() and cap_policy_rt_check() helpers
 * extracted from cap_policy_hotreload.c.  Runs on the host without seL4.
 *
 * Build:  cc -o /tmp/test_cap_policy_hotreload \
 *             tests/test_cap_policy_hotreload.c \
 *             -I kernel/agentos-root-task/include \
 *             -DAGENTOS_TEST_HOST
 * Run:    /tmp/test_cap_policy_hotreload
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Host-side stubs — replace Microkit primitives so the logic compiles and
 * runs on macOS / Linux without seL4.
 * ══════════════════════════════════════════════════════════════════════════ */
#ifdef AGENTOS_TEST_HOST

static uint64_t _mrs[64];
static inline void   microkit_mr_set(uint32_t i, uint64_t v) { _mrs[i] = v; }
static inline uint64_t microkit_mr_get(uint32_t i)           { return _mrs[i]; }

typedef uint32_t microkit_channel;
typedef uint64_t microkit_msginfo_t;
static inline microkit_msginfo_t microkit_msginfo_new(uint64_t label, uint32_t count) {
    (void)count; return label;
}
static inline microkit_msginfo_t microkit_ppcall(microkit_channel ch, microkit_msginfo_t m) {
    (void)ch; (void)m; return 0;
}
static inline void microkit_dbg_puts(const char *s) { printf("%s", s); }

/* Stubs for cap_broker externals referenced by cap_policy_hotreload.c */
void cap_broker_revoke_agent(uint32_t agent_pd, uint32_t reason_flags) {
    (void)agent_pd; (void)reason_flags;
}

static uint32_t _ext_count = 0;
typedef struct { bool active; uint32_t cap_kind; uint32_t cap_rights;
                 uint32_t owner_pd; uint32_t granted_to; } ExtCapEntry;
uint32_t cap_broker_ext_iter(uint32_t idx, ExtCapEntry *out) {
    (void)idx; (void)out; return 0;
}
uint32_t cap_broker_ext_count(void) { return _ext_count; }

/* power_ring_vaddr stub so power_mgr.c linkage doesn't break */
uintptr_t power_ring_vaddr = 0;

#define LOG(fmt, ...) printf("[test] " fmt "\n", ##__VA_ARGS__)

#endif /* AGENTOS_TEST_HOST */

/* ══════════════════════════════════════════════════════════════════════════
 * Inline copy of the CBOR parser and runtime policy from
 * cap_policy_hotreload.c so the test is self-contained.
 * ══════════════════════════════════════════════════════════════════════════ */

#define RT_POLICY_MAX   16u
#define NAME_MAX_LEN    32u

typedef struct {
    bool     active;
    char     class_name[NAME_MAX_LEN];
    uint32_t class_id;
    uint32_t caps_mask;
    uint32_t max_priority;
    uint32_t cpu_quota_ms;
    uint32_t mem_quota_kb;
    bool     allow_spawn;
    bool     allow_gpu;
} RtPolicyEntry;

static RtPolicyEntry g_rt_policy[RT_POLICY_MAX];
static uint32_t      g_rt_count  = 0;
static bool          g_rt_loaded = false;

/* ── Minimal CBOR primitives ─────────────────────────────────────────────── */

typedef struct { const uint8_t *p; uint32_t rem; } CborBuf;

static uint8_t cb_next(CborBuf *b) {
    if (b->rem == 0) return 0;
    b->rem--; return *b->p++;
}
static bool cb_ok(CborBuf *b) { return b->rem > 0; }

static bool cbor_read_head(CborBuf *b, uint8_t *major, uint64_t *arg) {
    if (!cb_ok(b)) return false;
    uint8_t ib = cb_next(b);
    *major = ib >> 5;
    uint8_t info = ib & 0x1F;
    if (info <= 23) { *arg = info; return true; }
    if (info == 24) { *arg = cb_next(b); return true; }
    if (info == 25) { uint16_t v = (uint16_t)cb_next(b) << 8 | cb_next(b);
                      *arg = v; return true; }
    if (info == 26) { uint32_t v = (uint32_t)cb_next(b) << 24 |
                                   (uint32_t)cb_next(b) << 16 |
                                   (uint32_t)cb_next(b) << 8  | cb_next(b);
                      *arg = v; return true; }
    return false;
}

static bool cbor_skip(CborBuf *b) {
    uint8_t maj; uint64_t arg;
    if (!cbor_read_head(b, &maj, &arg)) return false;
    if (maj == 0 || maj == 1 || maj == 7) return true;
    if (maj == 2 || maj == 3) { if (b->rem < arg) return false;
                                  b->p += (uint32_t)arg; b->rem -= (uint32_t)arg; return true; }
    if (maj == 4) { for (uint64_t i = 0; i < arg; i++) if (!cbor_skip(b)) return false; return true; }
    if (maj == 5) { for (uint64_t i = 0; i < arg * 2; i++) if (!cbor_skip(b)) return false; return true; }
    return false;
}

static bool cbor_read_text(CborBuf *b, char *dst, uint32_t dst_len) {
    uint8_t maj; uint64_t arg;
    if (!cbor_read_head(b, &maj, &arg)) return false;
    if (maj != 3) return false;
    uint32_t len = (uint32_t)arg;
    if (len >= dst_len || b->rem < len) return false;
    for (uint32_t i = 0; i < len; i++) dst[i] = (char)cb_next(b);
    dst[len] = '\0';
    return true;
}

static bool cbor_read_uint(CborBuf *b, uint32_t *out) {
    uint8_t maj; uint64_t arg;
    if (!cbor_read_head(b, &maj, &arg)) return false;
    if (maj != 0) return false;
    *out = (uint32_t)arg;
    return true;
}

static bool cbor_read_bool(CborBuf *b, bool *out) {
    uint8_t maj; uint64_t arg;
    if (!cbor_read_head(b, &maj, &arg)) return false;
    if (maj != 7) return false;
    if (arg == 20) { *out = false; return true; }
    if (arg == 21) { *out = true;  return true; }
    return false;
}

static uint32_t parse_policy_blob(const uint8_t *blob, uint32_t len) {
    CborBuf b = { .p = blob, .rem = len };
    uint8_t maj; uint64_t n_entries;
    if (!cbor_read_head(&b, &maj, &n_entries)) return 0;
    if (maj != 4) return 0;
    if (n_entries > RT_POLICY_MAX) n_entries = RT_POLICY_MAX;
    uint32_t count = 0;
    for (uint64_t ei = 0; ei < n_entries; ei++) {
        uint8_t emaj; uint64_t n_pairs;
        if (!cbor_read_head(&b, &emaj, &n_pairs)) break;
        if (emaj != 5) { cbor_skip(&b); continue; }
        RtPolicyEntry e = { .active = true };
        for (uint64_t pi = 0; pi < n_pairs; pi++) {
            char key[32] = {0};
            if (!cbor_read_text(&b, key, sizeof(key))) { cbor_skip(&b); continue; }
            if      (strcmp(key, "class")  == 0) cbor_read_text(&b, e.class_name, NAME_MAX_LEN);
            else if (strcmp(key, "id")     == 0) cbor_read_uint(&b, &e.class_id);
            else if (strcmp(key, "caps")   == 0) cbor_read_uint(&b, &e.caps_mask);
            else if (strcmp(key, "prio")   == 0) cbor_read_uint(&b, &e.max_priority);
            else if (strcmp(key, "cpu_ms") == 0) cbor_read_uint(&b, &e.cpu_quota_ms);
            else if (strcmp(key, "mem_kb") == 0) cbor_read_uint(&b, &e.mem_quota_kb);
            else if (strcmp(key, "spawn")  == 0) cbor_read_bool(&b, &e.allow_spawn);
            else if (strcmp(key, "gpu")    == 0) cbor_read_bool(&b, &e.allow_gpu);
            else cbor_skip(&b);
        }
        if (e.class_name[0] != '\0') g_rt_policy[count++] = e;
    }
    g_rt_count  = count;
    g_rt_loaded = (count > 0);
    return count;
}

static bool cap_policy_rt_check(uint32_t class_id, uint32_t requested_caps) {
    if (!g_rt_loaded) return true;
    for (uint32_t i = 0; i < g_rt_count; i++) {
        if (!g_rt_policy[i].active) continue;
        if (g_rt_policy[i].class_id != class_id) continue;
        return (g_rt_policy[i].caps_mask & requested_caps) == requested_caps;
    }
    return true;
}

/* ── CBOR blob builders (host-side only) ─────────────────────────────────── */

/* Append a CBOR byte to a dynamic buffer */
typedef struct { uint8_t *buf; uint32_t len; uint32_t cap; } CborWriter;

static void cw_push(CborWriter *w, uint8_t b) {
    if (w->len < w->cap) w->buf[w->len++] = b;
}
static void cw_uint(CborWriter *w, uint32_t v) {
    if      (v <= 23)   { cw_push(w, (uint8_t)v); }
    else if (v <= 0xFF) { cw_push(w, 0x18); cw_push(w, (uint8_t)v); }
    else {
        cw_push(w, 0x1A);
        cw_push(w, (uint8_t)(v >> 24)); cw_push(w, (uint8_t)(v >> 16));
        cw_push(w, (uint8_t)(v >>  8)); cw_push(w, (uint8_t) v);
    }
}
static void cw_text(CborWriter *w, const char *s) {
    uint32_t n = (uint32_t)strlen(s);
    if (n <= 23) cw_push(w, (uint8_t)(0x60 | n));
    else         { cw_push(w, 0x78); cw_push(w, (uint8_t)n); }
    for (uint32_t i = 0; i < n; i++) cw_push(w, (uint8_t)s[i]);
}
static void cw_bool(CborWriter *w, bool v) {
    cw_push(w, v ? 0xF5 : 0xF4);
}
static void cw_map_open(CborWriter *w, uint32_t pairs) {
    if (pairs <= 23) cw_push(w, (uint8_t)(0xA0 | pairs));
}
static void cw_array_open(CborWriter *w, uint32_t items) {
    if (items <= 23) cw_push(w, (uint8_t)(0x80 | items));
}

/* Build a policy blob with N entries, each with class="agentX", id=X */
static uint32_t build_policy(uint8_t *buf, uint32_t cap, uint32_t n_entries,
                              uint32_t caps_mask, bool allow_spawn, bool allow_gpu) {
    CborWriter w = { .buf = buf, .len = 0, .cap = cap };
    cw_array_open(&w, (uint32_t)n_entries);
    for (uint32_t i = 0; i < n_entries; i++) {
        uint8_t n_pairs = 8;
        cw_map_open(&w, n_pairs);
        /* class */
        cw_text(&w, "class");
        char name[16]; snprintf(name, sizeof(name), "agent%u", i);
        cw_text(&w, name);
        /* id */
        cw_text(&w, "id");   cw_uint(&w, i);
        /* caps */
        cw_text(&w, "caps"); cw_uint(&w, caps_mask);
        /* prio */
        cw_text(&w, "prio"); cw_uint(&w, 100u);
        /* cpu_ms */
        cw_text(&w, "cpu_ms"); cw_uint(&w, 1000u * (i + 1));
        /* mem_kb */
        cw_text(&w, "mem_kb"); cw_uint(&w, 512u * (i + 1));
        /* spawn */
        cw_text(&w, "spawn"); cw_bool(&w, allow_spawn);
        /* gpu */
        cw_text(&w, "gpu");   cw_bool(&w, allow_gpu);
    }
    return w.len;
}

/* ── Test framework ──────────────────────────────────────────────────────── */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("\n=== TEST: %s ===\n", (name))

#define ASSERT_TRUE(expr, msg) do { \
    if (!(expr)) { printf("  FAIL: %s\n", (msg)); tests_failed++; } \
    else         { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)

#define ASSERT_EQ(a, b, msg) do { \
    if ((uint64_t)(a) != (uint64_t)(b)) { \
        printf("  FAIL: %s (expected %llu, got %llu)\n", (msg), \
               (unsigned long long)(b), (unsigned long long)(a)); \
        tests_failed++; \
    } else { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)

/* ── Tests ────────────────────────────────────────────────────────────────── */

static void test_parse_empty_blob(void) {
    TEST("parse_empty_blob");
    uint8_t blob[4];
    CborWriter w = { .buf = blob, .len = 0, .cap = sizeof(blob) };
    cw_array_open(&w, 0);
    g_rt_loaded = false; g_rt_count = 0;
    uint32_t n = parse_policy_blob(blob, w.len);
    ASSERT_EQ(n, 0, "zero entries parsed from empty array");
    ASSERT_TRUE(!g_rt_loaded, "rt_loaded = false on empty blob");
}

static void test_parse_single_entry(void) {
    TEST("parse_single_entry");
    uint8_t blob[256];
    uint32_t len = build_policy(blob, sizeof(blob), 1, 0x0F, true, false);
    ASSERT_TRUE(len > 0, "blob builder produced bytes");
    g_rt_loaded = false; g_rt_count = 0;
    uint32_t n = parse_policy_blob(blob, len);
    ASSERT_EQ(n, 1, "one entry parsed");
    ASSERT_TRUE(g_rt_loaded, "rt_loaded = true after parse");
    ASSERT_EQ(g_rt_policy[0].class_id, 0, "class_id = 0");
    ASSERT_EQ(g_rt_policy[0].caps_mask, 0x0F, "caps_mask preserved");
    ASSERT_TRUE(g_rt_policy[0].allow_spawn, "allow_spawn = true");
    ASSERT_TRUE(!g_rt_policy[0].allow_gpu, "allow_gpu = false");
    ASSERT_EQ(g_rt_policy[0].cpu_quota_ms, 1000, "cpu_quota_ms = 1000");
    ASSERT_EQ(g_rt_policy[0].mem_quota_kb, 512, "mem_quota_kb = 512");
}

static void test_parse_multiple_entries(void) {
    TEST("parse_multiple_entries");
    uint8_t blob[1024];
    uint32_t n_entries = 4;
    uint32_t len = build_policy(blob, sizeof(blob), n_entries, 0xFF, false, true);
    g_rt_loaded = false; g_rt_count = 0;
    uint32_t n = parse_policy_blob(blob, len);
    ASSERT_EQ(n, n_entries, "all four entries parsed");
    for (uint32_t i = 0; i < n_entries; i++) {
        ASSERT_EQ(g_rt_policy[i].class_id, i, "class_id sequential");
        ASSERT_EQ(g_rt_policy[i].caps_mask, 0xFF, "caps_mask = 0xFF");
        ASSERT_TRUE(!g_rt_policy[i].allow_spawn, "allow_spawn = false");
        ASSERT_TRUE(g_rt_policy[i].allow_gpu, "allow_gpu = true");
    }
}

static void test_rt_check_no_policy(void) {
    TEST("rt_check_no_policy");
    g_rt_loaded = false; g_rt_count = 0;
    /* Without a loaded policy, everything is permitted (fall-through) */
    ASSERT_TRUE(cap_policy_rt_check(0, 0xFF), "no policy → all caps allowed");
    ASSERT_TRUE(cap_policy_rt_check(7, 0x01), "no policy → any class allowed");
}

static void test_rt_check_caps_allowed(void) {
    TEST("rt_check_caps_allowed");
    uint8_t blob[256];
    uint32_t len = build_policy(blob, sizeof(blob), 1, 0x0F, true, false);
    parse_policy_blob(blob, len);
    /* Request caps within mask — should pass */
    ASSERT_TRUE(cap_policy_rt_check(0, 0x01), "single bit within mask allowed");
    ASSERT_TRUE(cap_policy_rt_check(0, 0x0F), "exact mask allowed");
}

static void test_rt_check_caps_denied(void) {
    TEST("rt_check_caps_denied");
    uint8_t blob[256];
    uint32_t len = build_policy(blob, sizeof(blob), 1, 0x03, false, false);
    parse_policy_blob(blob, len);
    /* Request caps outside mask — should be denied */
    ASSERT_TRUE(!cap_policy_rt_check(0, 0x04), "bit outside mask denied");
    ASSERT_TRUE(!cap_policy_rt_check(0, 0xFF), "all caps denied when mask = 0x03");
}

static void test_rt_check_unknown_class(void) {
    TEST("rt_check_unknown_class");
    uint8_t blob[256];
    uint32_t len = build_policy(blob, sizeof(blob), 1, 0x0F, true, false);
    parse_policy_blob(blob, len);
    /* Class ID not in policy → conservative allow */
    ASSERT_TRUE(cap_policy_rt_check(99, 0xFF), "unknown class falls through to allow");
}

static void test_parse_max_entries(void) {
    TEST("parse_max_entries");
    uint8_t blob[4096];
    uint32_t len = build_policy(blob, sizeof(blob), RT_POLICY_MAX, 0x0F, false, false);
    g_rt_loaded = false; g_rt_count = 0;
    uint32_t n = parse_policy_blob(blob, len);
    ASSERT_EQ(n, RT_POLICY_MAX, "parser handles RT_POLICY_MAX entries");
}

static void test_parse_truncated_blob(void) {
    TEST("parse_truncated_blob");
    uint8_t blob[256];
    uint32_t full_len = build_policy(blob, sizeof(blob), 2, 0x0F, false, false);
    /* Feed only half the bytes — parser must not crash */
    g_rt_loaded = false; g_rt_count = 0;
    (void)parse_policy_blob(blob, full_len / 2);
    /* We don't assert a specific count — just that it didn't crash */
    ASSERT_TRUE(true, "truncated blob did not crash");
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  agentOS Cap-Policy Hot-Reload — Test Suite      ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");

    test_parse_empty_blob();
    test_parse_single_entry();
    test_parse_multiple_entries();
    test_rt_check_no_policy();
    test_rt_check_caps_allowed();
    test_rt_check_caps_denied();
    test_rt_check_unknown_class();
    test_parse_max_entries();
    test_parse_truncated_blob();

    printf("\n══════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    if (tests_failed > 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
