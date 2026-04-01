/*
 * test_cap_policy_reload.c — unit tests for agentOS cap_broker policy hot-reload
 *
 * Standalone host test that mirrors the policy parsing, grant-checking, and
 * reload logic from cap_broker.c without any seL4 / Microkit dependencies.
 *
 * Tests:
 *   1. Initial state: policy_version == 1, no active grants
 *   2. Grant a capability; verify it is marked active
 *   3. Reload policy that excludes that cap type for the grantee PD
 *   4. Verify the grant was revoked
 *   5. Verify policy_version incremented to 2
 *   6. Verify audit log has a CAP_AUDIT_POLICY_RELOAD event
 *   7. Malformed blob: policy unchanged, version stays at 2
 *   8. Wildcard cap_type (0): matching grant is NOT revoked
 *   9. Multiple grants: only the violating one is revoked
 *  10. Empty policy (n_rules == 0): all grants permitted, none revoked
 *
 * Build & run (no dependencies):
 *   cc test/test_cap_policy_reload.c -o /tmp/test_cap_policy_reload && \
 *       /tmp/test_cap_policy_reload
 *
 * Exits 0 on all pass, non-zero on first failure.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* ── Mirrored constants ──────────────────────────────────────────────────── */

#define MAX_CAPS             256
#define MAX_POLICY_RULES      32u
#define MAX_AGENTS_PER_RULE    8u
#define CAP_POLICY_BLOB_MAX 4096u

#define CAP_EVENT_GRANT          1u
#define CAP_EVENT_REVOKE         2u
#define CAP_AUDIT_POLICY_RELOAD  8u

/* ── Mirrored data structures ────────────────────────────────────────────── */

typedef struct {
    uint32_t cptr;
    uint32_t rights;
    uint32_t kind;
    uint32_t badge;
} sim_cap_desc_t;

typedef struct {
    bool          active;
    sim_cap_desc_t cap;
    uint32_t      owner_pd;
    uint32_t      granted_to;
    bool          revokable;
    uint64_t      grant_time;
} sim_cap_entry_t;

typedef struct {
    uint32_t cap_type;
    uint32_t rights_mask;
    uint32_t n_agents;
    uint32_t allowed_agents[MAX_AGENTS_PER_RULE];
} sim_policy_rule_t;

typedef struct {
    uint32_t          n_rules;
    sim_policy_rule_t rules[MAX_POLICY_RULES];
} sim_policy_t;

/* ── Simulation state ────────────────────────────────────────────────────── */

static sim_cap_entry_t sim_cap_table[MAX_CAPS];
static uint32_t        sim_cap_count   = 0;
static uint64_t        sim_audit_seq   = 0;
static sim_policy_t    sim_active_pol;
static sim_policy_t    sim_staging_pol;
static uint32_t        sim_policy_ver  = 1u;

/* ── Audit stub ──────────────────────────────────────────────────────────── */

#define SIM_AUDIT_MAX 64

typedef struct {
    uint32_t event_type;
    uint32_t agent_id;    /* grants_checked for POLICY_RELOAD */
    uint32_t caps_mask;   /* grants_revoked for POLICY_RELOAD */
    uint32_t slot_id;     /* policy_version for POLICY_RELOAD */
} sim_audit_event_t;

static sim_audit_event_t sim_audit[SIM_AUDIT_MAX];
static uint32_t          sim_audit_count = 0;

static void sim_audit_append(uint32_t ev, uint32_t ag, uint32_t cm, uint32_t sl) {
    if (sim_audit_count < SIM_AUDIT_MAX) {
        sim_audit[sim_audit_count].event_type = ev;
        sim_audit[sim_audit_count].agent_id   = ag;
        sim_audit[sim_audit_count].caps_mask  = cm;
        sim_audit[sim_audit_count].slot_id    = sl;
        sim_audit_count++;
    }
}

/* ── Helpers (mirror cap_broker.c logic) ─────────────────────────────────── */

static void sim_init(void) {
    memset(sim_cap_table,   0, sizeof(sim_cap_table));
    memset(&sim_active_pol, 0, sizeof(sim_active_pol));
    memset(&sim_staging_pol,0, sizeof(sim_staging_pol));
    memset(sim_audit,       0, sizeof(sim_audit));
    sim_cap_count    = 0;
    sim_audit_seq    = 0;
    sim_audit_count  = 0;
    sim_policy_ver   = 1u;
}

static int sim_register(uint32_t owner, uint32_t kind, uint32_t rights) {
    for (int i = 0; i < MAX_CAPS; i++) {
        if (!sim_cap_table[i].active) {
            sim_cap_table[i].active       = true;
            sim_cap_table[i].cap.kind     = kind;
            sim_cap_table[i].cap.rights   = rights;
            sim_cap_table[i].cap.cptr     = (uint32_t)i;
            sim_cap_table[i].owner_pd     = owner;
            sim_cap_table[i].granted_to   = 0;
            sim_cap_table[i].revokable    = true;
            sim_cap_table[i].grant_time   = 0;
            sim_cap_count++;
            return i;
        }
    }
    return -1;
}

static bool sim_grant(int handle, uint32_t to_pd) {
    if (handle < 0 || handle >= MAX_CAPS || !sim_cap_table[handle].active)
        return false;
    sim_cap_table[handle].granted_to = to_pd;
    sim_cap_table[handle].grant_time = ++sim_audit_seq;
    sim_audit_append(CAP_EVENT_GRANT, to_pd,
                     sim_cap_table[handle].cap.rights, (uint32_t)handle);
    return true;
}

/* Read LE uint32 */
static uint32_t pol_rd32(const uint8_t *p) {
    return (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16)| ((uint32_t)p[3] << 24);
}

static void pol_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int sim_policy_parse(const uint8_t *blob, uint32_t size,
                             sim_policy_t *out)
{
    if (!blob || size < 4u) return -1;
    uint32_t n = pol_rd32(blob);
    if (n > MAX_POLICY_RULES) return -1;
    uint32_t stride   = (3u + MAX_AGENTS_PER_RULE) * 4u;
    uint32_t expected = 4u + n * stride;
    if (size < expected) return -1;

    out->n_rules = n;
    const uint8_t *p = blob + 4u;
    for (uint32_t r = 0; r < n; r++) {
        sim_policy_rule_t *rule = &out->rules[r];
        rule->cap_type    = pol_rd32(p); p += 4;
        rule->rights_mask = pol_rd32(p); p += 4;
        rule->n_agents    = pol_rd32(p); p += 4;
        if (rule->n_agents > MAX_AGENTS_PER_RULE)
            rule->n_agents = MAX_AGENTS_PER_RULE;
        for (uint32_t a = 0; a < MAX_AGENTS_PER_RULE; a++) {
            rule->allowed_agents[a] = pol_rd32(p); p += 4;
        }
    }
    return 0;
}

static bool sim_check_grant(const sim_cap_entry_t *e, const sim_policy_t *pol) {
    if (pol->n_rules == 0) return true;
    for (uint32_t r = 0; r < pol->n_rules; r++) {
        const sim_policy_rule_t *rule = &pol->rules[r];
        if (rule->cap_type != 0 && rule->cap_type != e->cap.kind) continue;
        bool agent_ok = false;
        for (uint32_t a = 0; a < rule->n_agents && a < MAX_AGENTS_PER_RULE; a++) {
            if (rule->allowed_agents[a] == e->granted_to) { agent_ok = true; break; }
        }
        if (!agent_ok) continue;
        if ((e->cap.rights & ~rule->rights_mask) != 0) continue;
        return true;
    }
    return false;
}

static int sim_policy_reload(const uint8_t *blob, uint32_t size,
                              uint32_t *out_checked, uint32_t *out_revoked)
{
    if (size > CAP_POLICY_BLOB_MAX) return -1;
    if (sim_policy_parse(blob, size, &sim_staging_pol) != 0) return -1;

    uint32_t checked = 0u, revoked = 0u;
    for (int i = 0; i < MAX_CAPS; i++) {
        if (!sim_cap_table[i].active || sim_cap_table[i].granted_to == 0) continue;
        checked++;
        if (!sim_check_grant(&sim_cap_table[i], &sim_staging_pol)) {
            uint32_t grantee             = sim_cap_table[i].granted_to;
            sim_cap_table[i].granted_to  = 0;
            sim_cap_table[i].grant_time  = 0;
            sim_audit_seq++;
            revoked++;
            sim_audit_append(CAP_EVENT_REVOKE, grantee,
                             sim_cap_table[i].cap.rights, (uint32_t)i);
        }
    }

    /* Atomic swap */
    sim_active_pol = sim_staging_pol;
    sim_policy_ver++;

    /* Policy-reload audit event */
    sim_audit_append(CAP_AUDIT_POLICY_RELOAD, checked, revoked, sim_policy_ver);

    if (out_checked) *out_checked = checked;
    if (out_revoked) *out_revoked = revoked;
    return 0;
}

/* ── Blob builder helper ─────────────────────────────────────────────────── */

/*
 * build_policy_blob — serialise up to n_rules rules into buf.
 * Each rule: cap_type, rights_mask, n_agents, then MAX_AGENTS_PER_RULE agent slots.
 * Returns byte length written.
 */
static uint32_t build_policy_blob(uint8_t *buf, uint32_t buf_sz,
                                   uint32_t n_rules,
                                   uint32_t *cap_types,
                                   uint32_t *rights_masks,
                                   uint32_t *n_agents_arr,
                                   uint32_t agents[][MAX_AGENTS_PER_RULE])
{
    uint32_t stride = (3u + MAX_AGENTS_PER_RULE) * 4u;
    uint32_t total  = 4u + n_rules * stride;
    if (total > buf_sz) return 0;

    uint8_t *p = buf;
    pol_wr32(p, n_rules); p += 4;
    for (uint32_t r = 0; r < n_rules; r++) {
        pol_wr32(p, cap_types[r]);    p += 4;
        pol_wr32(p, rights_masks[r]); p += 4;
        pol_wr32(p, n_agents_arr[r]); p += 4;
        for (uint32_t a = 0; a < MAX_AGENTS_PER_RULE; a++) {
            pol_wr32(p, agents[r][a]); p += 4;
        }
    }
    return total;
}

/* ── Test harness ────────────────────────────────────────────────────────── */

#define PASS() do { printf("  PASS\n"); } while(0)
#define FAIL(msg) do { printf("  FAIL: %s\n", msg); return false; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) FAIL(msg); } while(0)

static bool test_initial_state(void) {
    printf("Test 1: initial state\n");
    sim_init();
    CHECK(sim_policy_ver == 1u, "policy_version should start at 1");
    CHECK(sim_cap_count  == 0,  "no caps registered yet");
    CHECK(sim_active_pol.n_rules == 0, "default policy has 0 rules (allow-all)");
    PASS();
    return true;
}

static bool test_grant_visible(void) {
    printf("Test 2: grant a capability\n");
    sim_init();
    int h = sim_register(1, /*kind=*/0x10, /*rights=*/0x07);
    CHECK(h >= 0, "register should succeed");
    bool ok = sim_grant(h, /*to_pd=*/42);
    CHECK(ok, "grant should succeed");
    CHECK(sim_cap_table[h].granted_to == 42, "granted_to should be 42");
    PASS();
    return true;
}

static bool test_reload_revokes_violating_grant(void) {
    printf("Test 3+4: reload policy that excludes cap type for grantee\n");
    sim_init();

    /* Grant cap kind=0x10, rights=0x07 to PD 42 */
    int h = sim_register(1, 0x10, 0x07);
    sim_grant(h, 42);
    CHECK(sim_cap_table[h].granted_to == 42, "pre-condition: grant active");

    /* Policy: kind=0x10 is allowed only for PD 99 (not 42) */
    uint8_t blob[512];
    uint32_t cap_types[1]       = { 0x10 };
    uint32_t rights_masks[1]    = { 0xFF };
    uint32_t n_agents_arr[1]    = { 1 };
    uint32_t agents[1][MAX_AGENTS_PER_RULE];
    memset(agents, 0, sizeof(agents));
    agents[0][0] = 99;  /* only PD 99 is allowed */

    uint32_t blen = build_policy_blob(blob, sizeof(blob),
                                       1, cap_types, rights_masks,
                                       n_agents_arr, agents);
    CHECK(blen > 0, "blob build should succeed");

    uint32_t checked = 0, revoked = 0;
    int rc = sim_policy_reload(blob, blen, &checked, &revoked);
    CHECK(rc == 0,  "reload should return 0");
    CHECK(checked == 1, "one active grant should be checked");
    CHECK(revoked  == 1, "grant for PD 42 should be revoked");
    CHECK(sim_cap_table[h].granted_to == 0, "grant should be cleared");

    PASS();
    return true;
}

static bool test_policy_version_incremented(void) {
    printf("Test 5: policy_version increments on reload\n");
    sim_init();
    CHECK(sim_policy_ver == 1u, "starts at 1");

    /* Reload with empty policy (0 rules) */
    uint8_t blob[4];
    pol_wr32(blob, 0);  /* n_rules = 0 */
    int rc = sim_policy_reload(blob, 4, NULL, NULL);
    CHECK(rc == 0, "reload of empty policy should succeed");
    CHECK(sim_policy_ver == 2u, "version should be 2 after first reload");

    sim_policy_reload(blob, 4, NULL, NULL);
    CHECK(sim_policy_ver == 3u, "version should be 3 after second reload");

    PASS();
    return true;
}

static bool test_audit_has_policy_reload_event(void) {
    printf("Test 6: audit log contains CAP_AUDIT_POLICY_RELOAD event\n");
    sim_init();

    uint8_t blob[4];
    pol_wr32(blob, 0);
    sim_policy_reload(blob, 4, NULL, NULL);

    bool found = false;
    for (uint32_t i = 0; i < sim_audit_count; i++) {
        if (sim_audit[i].event_type == CAP_AUDIT_POLICY_RELOAD) {
            found = true;
            CHECK(sim_audit[i].slot_id == sim_policy_ver,
                  "slot_id should equal new policy_version");
            break;
        }
    }
    CHECK(found, "CAP_AUDIT_POLICY_RELOAD event not found in audit log");

    PASS();
    return true;
}

static bool test_bad_blob_leaves_policy_unchanged(void) {
    printf("Test 7: malformed blob leaves policy unchanged\n");
    sim_init();

    /* Grant a cap */
    int h = sim_register(1, 0x20, 0x01);
    sim_grant(h, 7);

    uint32_t ver_before = sim_policy_ver;
    uint32_t rules_before = sim_active_pol.n_rules;

    /* Too-short blob */
    uint8_t bad[3] = { 0x01, 0x00, 0x00 };
    int rc = sim_policy_reload(bad, 3, NULL, NULL);
    CHECK(rc == -1, "reload of bad blob should return -1");
    CHECK(sim_policy_ver == ver_before, "policy_version should not change");
    CHECK(sim_active_pol.n_rules == rules_before, "active policy should not change");
    CHECK(sim_cap_table[h].granted_to == 7, "grant should still be active");

    PASS();
    return true;
}

static bool test_wildcard_cap_type_does_not_revoke(void) {
    printf("Test 8: wildcard cap_type (0) with matching agent — grant kept\n");
    sim_init();

    int h = sim_register(1, 0x30, 0x03);
    sim_grant(h, 5);

    /* Policy: wildcard type (0), rights 0xFF, agent 5 allowed */
    uint8_t blob[512];
    uint32_t cap_types[1]    = { 0 };   /* wildcard */
    uint32_t rights_masks[1] = { 0xFF };
    uint32_t n_agents_arr[1] = { 1 };
    uint32_t agents[1][MAX_AGENTS_PER_RULE];
    memset(agents, 0, sizeof(agents));
    agents[0][0] = 5;

    uint32_t blen = build_policy_blob(blob, sizeof(blob),
                                       1, cap_types, rights_masks,
                                       n_agents_arr, agents);

    uint32_t revoked = 0;
    int rc = sim_policy_reload(blob, blen, NULL, &revoked);
    CHECK(rc == 0,     "reload should succeed");
    CHECK(revoked == 0, "no grant should be revoked");
    CHECK(sim_cap_table[h].granted_to == 5, "grant for PD 5 should remain");

    PASS();
    return true;
}

static bool test_multiple_grants_only_violating_revoked(void) {
    printf("Test 9: multiple grants — only violating one revoked\n");
    sim_init();

    /* Two caps of different kinds, both granted */
    int h1 = sim_register(1, 0x10, 0x01);  /* kind 0x10, PD 10 */
    int h2 = sim_register(1, 0x20, 0x01);  /* kind 0x20, PD 20 */
    sim_grant(h1, 10);
    sim_grant(h2, 20);

    /* Policy: allow kind=0x10 for PD 10, nothing else */
    uint8_t blob[512];
    uint32_t cap_types[1]    = { 0x10 };
    uint32_t rights_masks[1] = { 0xFF };
    uint32_t n_agents_arr[1] = { 1 };
    uint32_t agents[1][MAX_AGENTS_PER_RULE];
    memset(agents, 0, sizeof(agents));
    agents[0][0] = 10;

    uint32_t blen = build_policy_blob(blob, sizeof(blob),
                                       1, cap_types, rights_masks,
                                       n_agents_arr, agents);

    uint32_t checked = 0, revoked = 0;
    int rc = sim_policy_reload(blob, blen, &checked, &revoked);
    CHECK(rc == 0,       "reload should succeed");
    CHECK(checked == 2,  "two active grants checked");
    CHECK(revoked  == 1, "only the kind=0x20 grant should be revoked");
    CHECK(sim_cap_table[h1].granted_to == 10, "kind=0x10 grant for PD 10 kept");
    CHECK(sim_cap_table[h2].granted_to == 0,  "kind=0x20 grant for PD 20 revoked");

    PASS();
    return true;
}

static bool test_empty_policy_allows_all(void) {
    printf("Test 10: empty policy (0 rules) — no grants revoked\n");
    sim_init();

    int h1 = sim_register(1, 0xAA, 0xFF);
    int h2 = sim_register(1, 0xBB, 0xFF);
    sim_grant(h1, 100);
    sim_grant(h2, 200);

    uint8_t blob[4];
    pol_wr32(blob, 0);  /* n_rules = 0 */

    uint32_t revoked = 0;
    int rc = sim_policy_reload(blob, 4, NULL, &revoked);
    CHECK(rc == 0,      "reload should succeed");
    CHECK(revoked == 0, "empty policy = allow all, zero revocations");
    CHECK(sim_cap_table[h1].granted_to == 100, "grant for PD 100 kept");
    CHECK(sim_cap_table[h2].granted_to == 200, "grant for PD 200 kept");

    PASS();
    return true;
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== agentOS cap_broker policy hot-reload tests ===\n");

    int failures = 0;
    failures += !test_initial_state();
    failures += !test_grant_visible();
    failures += !test_reload_revokes_violating_grant();
    failures += !test_policy_version_incremented();
    failures += !test_audit_has_policy_reload_event();
    failures += !test_bad_blob_leaves_policy_unchanged();
    failures += !test_wildcard_cap_type_does_not_revoke();
    failures += !test_multiple_grants_only_violating_revoked();
    failures += !test_empty_policy_allows_all();

    printf("\n%d test(s) failed.\n", failures);
    return failures ? 1 : 0;
}
