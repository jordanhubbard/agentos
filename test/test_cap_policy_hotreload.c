/*
 * test_cap_policy_hotreload.c — unit tests for agentOS cap_policy hot-reload
 *
 * Standalone host test that mirrors the CBOR parsing, runtime policy table
 * management, grant revalidation, and diff logic from cap_policy_hotreload.c
 * without any seL4 / Microkit dependencies.
 *
 * Tests:
 *   1. Initial state: rt_loaded=false, version=0
 *   2. CBOR parse: minimal valid policy blob parses to correct table
 *   3. Policy check: grants within new policy pass, excess caps fail
 *   4. Hash dedup: identical blob does not increment version
 *   5. Reset: rt_loaded reverts to false, version increments
 *   6. Revocation count: active grants violating new policy counted correctly
 *   7. Multi-class: multiple class entries parsed and checked independently
 *   8. Truncated blob: graceful failure, state unchanged
 *
 * Build & run:
 *   cc test/test_cap_policy_hotreload.c -o /tmp/test_cap_policy_hotreload && /tmp/test_cap_policy_hotreload
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* ── Mirror constants ─────────────────────────────────────────────────────── */

#define OP_CAP_POLICY_RELOAD   0xC0u
#define OP_CAP_POLICY_STATUS   0xC1u
#define OP_CAP_POLICY_RESET    0xC2u
#define OP_CAP_POLICY_DIFF     0xC3u

#define RT_POLICY_MAX  16u
#define NAME_MAX_LEN   32u

/* ── Mirrored data structures ────────────────────────────────────────────── */

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

/* ── Simulated state ─────────────────────────────────────────────────────── */

static RtPolicyEntry g_rt_policy[RT_POLICY_MAX];
static uint32_t      g_rt_count       = 0;
static bool          g_rt_loaded      = false;
static uint32_t      g_policy_version = 0;
static uint32_t      g_policy_hash    = 0;
static uint32_t      g_last_revoked   = 0;

/* ── Minimal djb2 ────────────────────────────────────────────────────────── */

static uint32_t djb2(const uint8_t *buf, uint32_t len) {
    uint32_t hash = 5381;
    for (uint32_t i = 0; i < len; i++)
        hash = ((hash << 5) + hash) ^ buf[i];
    return hash;
}

/* ── Minimal CBOR builder (for test blob construction) ───────────────────── */

typedef struct { uint8_t *p; uint32_t len; uint32_t cap; } CborOut;

static void cbo_byte(CborOut *o, uint8_t b) {
    if (o->len < o->cap) o->p[o->len++] = b;
}

static void cbo_uint(CborOut *o, uint32_t v) {
    if (v <= 23)        { cbo_byte(o, (uint8_t)v); }
    else if (v <= 0xFF) { cbo_byte(o, 24); cbo_byte(o, (uint8_t)v); }
    else                { cbo_byte(o, 26); cbo_byte(o, (uint8_t)(v>>24)); cbo_byte(o, (uint8_t)(v>>16)); cbo_byte(o, (uint8_t)(v>>8)); cbo_byte(o, (uint8_t)v); }
}

static void cbo_text(CborOut *o, const char *s) {
    uint32_t len = (uint32_t)strlen(s);
    if (len <= 23) cbo_byte(o, 0x60u | len);
    else { cbo_byte(o, 0x78u); cbo_byte(o, (uint8_t)len); }
    for (uint32_t i = 0; i < len; i++) cbo_byte(o, (uint8_t)s[i]);
}

static void cbo_bool(CborOut *o, bool v) { cbo_byte(o, v ? 0xF5u : 0xF4u); }

/* Build a test CBOR policy blob with N classes */
static uint32_t build_policy_blob(uint8_t *buf, uint32_t cap,
                                   uint32_t n_classes,
                                   uint32_t *class_ids,
                                   uint32_t *caps_masks,
                                   const char **class_names) {
    CborOut o = { .p = buf, .len = 0, .cap = cap };
    /* array of N maps */
    if (n_classes <= 23) cbo_byte(&o, 0x80u | (uint8_t)n_classes);
    for (uint32_t i = 0; i < n_classes; i++) {
        /* map with 8 entries */
        cbo_byte(&o, 0xA0u | 8u); /* CBOR map of 8 pairs */
        cbo_text(&o, "class");  cbo_text(&o, class_names[i]);
        cbo_text(&o, "id");     cbo_uint(&o, class_ids[i]);
        cbo_text(&o, "caps");   cbo_uint(&o, caps_masks[i]);
        cbo_text(&o, "prio");   cbo_uint(&o, 128);
        cbo_text(&o, "cpu_ms"); cbo_uint(&o, 10000);
        cbo_text(&o, "mem_kb"); cbo_uint(&o, 16384);
        cbo_text(&o, "spawn");  cbo_bool(&o, false);
        cbo_text(&o, "gpu");    cbo_bool(&o, class_ids[i] == 1);
    }
    return o.len;
}

/* ── Mirrored CBOR parser (same logic as cap_policy_hotreload.c) ─────────── */

typedef struct { const uint8_t *p; uint32_t rem; } CborBuf;
static uint8_t cb_next(CborBuf *b) { if (!b->rem) return 0; b->rem--; return *b->p++; }
static bool cbor_read_head(CborBuf *b, uint8_t *maj, uint64_t *arg) {
    if (!b->rem) return false;
    uint8_t ib = cb_next(b); *maj = ib >> 5;
    uint8_t info = ib & 0x1F;
    if (info <= 23) { *arg = info; return true; }
    if (info == 24) { *arg = cb_next(b); return true; }
    if (info == 25) { uint16_t v = (uint16_t)cb_next(b) << 8 | cb_next(b); *arg = v; return true; }
    if (info == 26) { uint32_t v = (uint32_t)cb_next(b)<<24|(uint32_t)cb_next(b)<<16|(uint32_t)cb_next(b)<<8|cb_next(b); *arg = v; return true; }
    return false;
}
static bool cbor_skip(CborBuf *b) {
    uint8_t m; uint64_t a;
    if (!cbor_read_head(b, &m, &a)) return false;
    if (m == 0 || m == 1 || m == 7) return true;
    if (m == 2 || m == 3) { if (b->rem < a) return false; b->p += a; b->rem -= (uint32_t)a; return true; }
    if (m == 4) { for (uint64_t i=0;i<a;i++) if(!cbor_skip(b)) return false; return true; }
    if (m == 5) { for (uint64_t i=0;i<a*2;i++) if(!cbor_skip(b)) return false; return true; }
    return false;
}
static bool cbor_read_text(CborBuf *b, char *dst, uint32_t n) {
    uint8_t m; uint64_t a;
    if (!cbor_read_head(b,&m,&a)||m!=3) return false;
    uint32_t l=(uint32_t)a; if(l>=n||b->rem<l) return false;
    for(uint32_t i=0;i<l;i++) dst[i]=(char)cb_next(b);
    dst[l]=0; return true;
}
static bool cbor_read_uint(CborBuf *b, uint32_t *o) {
    uint8_t m; uint64_t a;
    if (!cbor_read_head(b,&m,&a)||m!=0) return false;
    *o=(uint32_t)a; return true;
}
static bool cbor_read_bool(CborBuf *b, bool *o) {
    uint8_t m; uint64_t a;
    if (!cbor_read_head(b,&m,&a)||m!=7) return false;
    if (a==20) { *o=false; return true; }
    if (a==21) { *o=true;  return true; }
    return false;
}

/* Parse blob into g_rt_policy; returns count */
static uint32_t sim_parse(const uint8_t *blob, uint32_t len) {
    CborBuf b = { .p=blob, .rem=len };
    uint8_t maj; uint64_t n;
    if (!cbor_read_head(&b,&maj,&n)||maj!=4) return 0;
    if (n>RT_POLICY_MAX) n=RT_POLICY_MAX;
    uint32_t cnt=0;
    for (uint64_t ei=0;ei<n;ei++) {
        uint8_t em; uint64_t np;
        if (!cbor_read_head(&b,&em,&np)||em!=5) { cbor_skip(&b); continue; }
        RtPolicyEntry e = { .active=true };
        for (uint64_t pi=0;pi<np;pi++) {
            char key[32];
            if (!cbor_read_text(&b,key,sizeof(key))) { cbor_skip(&b); continue; }
            if      (!strcmp(key,"class"))  cbor_read_text(&b,e.class_name,NAME_MAX_LEN);
            else if (!strcmp(key,"id"))     cbor_read_uint(&b,&e.class_id);
            else if (!strcmp(key,"caps"))   cbor_read_uint(&b,&e.caps_mask);
            else if (!strcmp(key,"prio"))   cbor_read_uint(&b,&e.max_priority);
            else if (!strcmp(key,"cpu_ms")) cbor_read_uint(&b,&e.cpu_quota_ms);
            else if (!strcmp(key,"mem_kb")) cbor_read_uint(&b,&e.mem_quota_kb);
            else if (!strcmp(key,"spawn"))  cbor_read_bool(&b,&e.allow_spawn);
            else if (!strcmp(key,"gpu"))    cbor_read_bool(&b,&e.allow_gpu);
            else cbor_skip(&b);
        }
        if (e.class_name[0]) g_rt_policy[cnt++]=e;
    }
    return cnt;
}

/* Simulate a full hot-reload cycle */
static uint32_t sim_reload(const uint8_t *blob, uint32_t len) {
    if (!len) return 0;
    uint32_t h = djb2(blob, len);
    if (h == g_policy_hash && g_rt_loaded) return 0; /* dedup */
    uint32_t cnt = sim_parse(blob, len);
    if (!cnt) return 0;
    g_rt_count    = cnt;
    g_rt_loaded   = true;
    g_policy_hash = h;
    g_policy_version++;
    return cnt;
}

static void sim_reset(void) {
    memset(g_rt_policy, 0, sizeof(g_rt_policy));
    g_rt_count = 0; g_rt_loaded = false; g_policy_version++;
}

static bool sim_check(uint32_t class_id, uint32_t caps) {
    if (!g_rt_loaded) return true;
    for (uint32_t i=0;i<g_rt_count;i++) {
        if (!g_rt_policy[i].active) continue;
        if (g_rt_policy[i].class_id != class_id) continue;
        return (g_rt_policy[i].caps_mask & caps) == caps;
    }
    return true;
}

/* ── Test helpers ─────────────────────────────────────────────────────────── */

static int failures = 0;
#define PASS(n)       printf("PASS: %s\n", n)
#define FAIL(n, ...)  do { printf("FAIL: %s — ", n); printf(__VA_ARGS__); putchar('\n'); failures++; } while(0)

/* ── Test 1: Initial state ────────────────────────────────────────────────── */
static void test_initial_state(void) {
    memset(g_rt_policy, 0, sizeof(g_rt_policy));
    g_rt_count = 0; g_rt_loaded = false; g_policy_version = 0; g_policy_hash = 0;
    assert(!g_rt_loaded && g_rt_count == 0 && g_policy_version == 0);
    PASS("initial_state");
}

/* ── Test 2: CBOR parse ───────────────────────────────────────────────────── */
static void test_cbor_parse(void) {
    memset(g_rt_policy, 0, sizeof(g_rt_policy));
    g_rt_count = 0; g_rt_loaded = false;

    uint8_t blob[512]; uint32_t class_ids[] = {0,1};
    uint32_t caps[]    = {0x3F, 0x04}; /* system=all, compute=GPU only */
    const char *names[] = {"system","compute"};
    uint32_t len = build_policy_blob(blob, sizeof(blob), 2, class_ids, caps, names);
    uint32_t cnt = sim_parse(blob, len);
    if (cnt != 2) { FAIL("cbor_parse", "expected 2 entries, got %u", cnt); return; }
    if (strcmp(g_rt_policy[0].class_name, "system") != 0) {
        FAIL("cbor_parse", "expected class_name='system', got '%s'", g_rt_policy[0].class_name); return; }
    if (g_rt_policy[0].caps_mask != 0x3F) {
        FAIL("cbor_parse", "expected caps=0x3F, got 0x%x", g_rt_policy[0].caps_mask); return; }
    if (g_rt_policy[1].class_id != 1) {
        FAIL("cbor_parse", "expected class_id=1, got %u", g_rt_policy[1].class_id); return; }
    if (!g_rt_policy[1].allow_gpu) { FAIL("cbor_parse", "compute should have allow_gpu=true"); return; }
    PASS("cbor_parse: 2 entries parsed with correct fields");
}

/* ── Test 3: Policy check ─────────────────────────────────────────────────── */
static void test_policy_check(void) {
    memset(g_rt_policy, 0, sizeof(g_rt_policy));
    g_rt_count = 0; g_rt_loaded = false;
    uint8_t blob[512]; uint32_t cids[] = {1};
    uint32_t caps_v[] = {0x04}; /* compute: GPU only */
    const char *names[] = {"compute"};
    uint32_t len = build_policy_blob(blob, sizeof(blob), 1, cids, caps_v, names);
    sim_reload(blob, len);

    /* GPU cap (0x04) should pass for class 1 */
    if (!sim_check(1, 0x04)) { FAIL("policy_check", "GPU cap should be allowed for compute"); return; }
    /* FS cap (0x01) should fail for class 1 */
    if (sim_check(1, 0x01))  { FAIL("policy_check", "FS cap should be denied for compute"); return; }
    /* Unknown class (2) — not in table — should pass through */
    if (!sim_check(2, 0xFF)) { FAIL("policy_check", "unknown class should fall through (allow)"); return; }
    PASS("policy_check: allowed, denied, and unknown-class cases correct");
}

/* ── Test 4: Hash dedup ───────────────────────────────────────────────────── */
static void test_hash_dedup(void) {
    memset(g_rt_policy, 0, sizeof(g_rt_policy));
    g_rt_count = 0; g_rt_loaded = false; g_policy_version = 0; g_policy_hash = 0;
    uint8_t blob[512]; uint32_t cids[] = {0};
    uint32_t caps_v[] = {0xFF}; const char *names[] = {"system"};
    uint32_t len = build_policy_blob(blob, sizeof(blob), 1, cids, caps_v, names);
    sim_reload(blob, len);
    uint32_t v1 = g_policy_version;
    /* Reload same blob — should not increment version */
    sim_reload(blob, len);
    if (g_policy_version != v1) {
        FAIL("hash_dedup", "version incremented on identical blob (%u→%u)", v1, g_policy_version); return; }
    PASS("hash_dedup: identical blob does not increment version");
}

/* ── Test 5: Reset ────────────────────────────────────────────────────────── */
static void test_reset(void) {
    memset(g_rt_policy, 0, sizeof(g_rt_policy));
    g_rt_count = 0; g_rt_loaded = false; g_policy_version = 2; g_policy_hash = 0;
    uint8_t blob[512]; uint32_t cids[] = {0};
    uint32_t caps_v[] = {0x3F}; const char *names[] = {"system"};
    uint32_t len = build_policy_blob(blob, sizeof(blob), 1, cids, caps_v, names);
    sim_reload(blob, len);
    if (!g_rt_loaded) { FAIL("reset", "expected rt_loaded=true after reload (pre-reset)"); return; }
    uint32_t v_before = g_policy_version;
    sim_reset();
    if (g_rt_loaded) { FAIL("reset", "rt_loaded should be false after reset"); return; }
    if (g_rt_count)  { FAIL("reset", "rt_count should be 0 after reset"); return; }
    if (g_policy_version <= v_before) { FAIL("reset", "version should increment on reset"); return; }
    PASS("reset: rt_loaded=false, rt_count=0, version incremented");
}

/* ── Test 6: Revocation simulation ───────────────────────────────────────── */
static void test_revocation_count(void) {
    /* Simulate 4 active grants: 2 GPU (class 1), 2 FS (class 0) */
    /* New policy: compute class gets GPU only, system gets all */
    memset(g_rt_policy, 0, sizeof(g_rt_policy));
    g_rt_count = 0; g_rt_loaded = false; g_policy_hash = 0;
    uint8_t blob[512];
    uint32_t cids[]   = {0, 1};
    uint32_t caps_v[] = {0xFF, 0x04};       /* system=all, compute=GPU */
    const char *names[] = {"system","compute"};
    uint32_t len = build_policy_blob(blob, sizeof(blob), 2, cids, caps_v, names);
    sim_reload(blob, len);

    /* Simulate: compute agent (class 1) has FS cap (0x01) — violates new policy */
    uint32_t revoked = 0;
    typedef struct { uint32_t class_id; uint32_t caps; } SimGrant;
    SimGrant grants[] = {
        {1, 0x04}, /* GPU — ok */
        {1, 0x01}, /* FS  — violates */
        {0, 0xFF}, /* system all — ok */
        {1, 0x03}, /* GPU+FS — violates */
    };
    for (int i = 0; i < 4; i++) {
        if (!sim_check(grants[i].class_id, grants[i].caps)) revoked++;
    }
    if (revoked != 2) {
        FAIL("revocation_count", "expected 2 revocations, got %u", revoked); return; }
    PASS("revocation_count: 2 of 4 grants correctly identified for revocation");
}

/* ── Test 7: Multi-class ─────────────────────────────────────────────────── */
static void test_multi_class(void) {
    memset(g_rt_policy, 0, sizeof(g_rt_policy));
    g_rt_count = 0; g_rt_loaded = false; g_policy_hash = 0;
    uint8_t blob[1024];
    uint32_t cids[]   = {0, 1, 2};
    uint32_t caps_v[] = {0xFF, 0x04, 0x36}; /* system, compute, interactive */
    const char *names[] = {"system","compute","interactive"};
    uint32_t len = build_policy_blob(blob, sizeof(blob), 3, cids, caps_v, names);
    uint32_t cnt = sim_reload(blob, len);
    if (cnt != 3) { FAIL("multi_class", "expected 3 classes, got %u", cnt); return; }
    if (!sim_check(0, 0xFF)) { FAIL("multi_class", "system should allow all"); return; }
    if (sim_check(1, 0x01))  { FAIL("multi_class", "compute should deny FS"); return; }
    if (!sim_check(2, 0x02)) { FAIL("multi_class", "interactive should allow NET"); return; }
    PASS("multi_class: 3 classes parsed and checked independently");
}

/* ── Test 8: Truncated blob ──────────────────────────────────────────────── */
static void test_truncated_blob(void) {
    memset(g_rt_policy, 0, sizeof(g_rt_policy));
    g_rt_count = 0; g_rt_loaded = false; g_policy_hash = 0; g_policy_version = 5;
    uint8_t blob[512]; uint32_t cids[] = {0};
    uint32_t caps_v[] = {0xFF}; const char *names[] = {"system"};
    uint32_t len = build_policy_blob(blob, sizeof(blob), 1, cids, caps_v, names);
    /* Truncate to 5 bytes — too short to parse */
    uint32_t result = sim_parse(blob, 5);
    if (result != 0) { FAIL("truncated_blob", "expected 0 entries from truncated blob, got %u", result); return; }
    if (g_rt_loaded) { FAIL("truncated_blob", "rt_loaded should stay false"); return; }
    PASS("truncated_blob: truncated blob gracefully returns 0 entries");
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void) {
    printf("=== cap_policy_hotreload unit tests ===\n\n");
    test_initial_state();
    test_cbor_parse();
    test_policy_check();
    test_hash_dedup();
    test_reset();
    test_revocation_count();
    test_multi_class();
    test_truncated_blob();
    printf("\n=== Results: %d test(s) failed ===\n", failures);
    return failures ? 1 : 0;
}
