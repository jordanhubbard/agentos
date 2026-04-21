/*
 * test_capstore.c — API tests for the agentOS CapabilityBroker service
 *
 * Covered opcodes:
 *   OP_CAP_GRANT    (0x20) — issue a capability grant
 *   OP_CAP_CHECK    (0x21) — check whether a grant is valid
 *   OP_CAP_REVOKE   (0x22) — revoke a grant by token
 *   OP_CAP_QUERY    (0x23) — list active grants for an agent
 *   OP_CAP_AUDIT    (0x24) — retrieve audit log entries
 *   unknown opcode          — must return AOS_ERR_UNIMPL
 *
 * TODO: replace inline mock with
 *       #include "../../contracts/capability-broker/interface.h"
 *
 * Build & run:
 *   cc -DAGENTOS_TEST_HOST -I tests/api -o /tmp/t_capstore \
 *       tests/api/test_capstore.c && /tmp/t_capstore
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST
#include "framework.h"

/* ── Opcode definitions ───────────────────────────────────────────────────── */
/* TODO: replace with #include "../../contracts/capability-broker/interface.h" */
#define OP_CAP_GRANT    0x20u
#define OP_CAP_CHECK    0x21u
#define OP_CAP_REVOKE   0x22u
#define OP_CAP_QUERY    0x23u
#define OP_CAP_AUDIT    0x24u

/* ── Rights bitmask (matches sdk/src/capability.rs) ──────────────────────── */
#define CAP_RIGHT_READ    0x01u
#define CAP_RIGHT_WRITE   0x02u
#define CAP_RIGHT_EXECUTE 0x04u
#define CAP_RIGHT_GRANT   0x08u
#define CAP_RIGHT_REVOKE  0x10u
#define CAP_RIGHTS_ALL    0x1Fu
#define CAP_RIGHTS_NONE   0x00u

/* ── Capability kinds ─────────────────────────────────────────────────────── */
#define CAP_KIND_MEMORY      0u
#define CAP_KIND_ENDPOINT    1u
#define CAP_KIND_NOTIFICATION 2u
#define CAP_KIND_FRAME       3u
#define CAP_KIND_CUSTOM      0xFFu

/* ── Mock CapabilityBroker ────────────────────────────────────────────────── */

#define MOCK_MAX_GRANTS     32u
#define MOCK_MAX_AUDIT      64u
#define MOCK_AGENT_LEN      32u

typedef struct {
    uint64_t token;          /* opaque grant token (1-based) */
    uint32_t cap_kind;
    uint32_t rights;
    char     grantor[MOCK_AGENT_LEN];
    char     grantee[MOCK_AGENT_LEN];
    uint32_t delegatable;
    uint32_t active;
} MockGrant;

typedef struct {
    uint32_t op;             /* 0=grant 1=check 2=revoke */
    uint32_t result;         /* 0=allowed 1=denied 2=not_found */
    char     agent_id[MOCK_AGENT_LEN];
    uint32_t cap_kind;
} MockAuditEntry;

static MockGrant      g_grants[MOCK_MAX_GRANTS];
static MockAuditEntry g_audit[MOCK_MAX_AUDIT];
static uint32_t       g_audit_count = 0;
static uint64_t       g_next_token  = 1u;

/* Per-grantor policy table: which (grantor, cap_kind) pairs are allowed */
typedef struct { char grantor[MOCK_AGENT_LEN]; uint32_t cap_kind; } MockPolicy;
static MockPolicy g_policy[16];
static uint32_t   g_policy_count = 0;

static void cap_reset(void) {
    memset(g_grants,  0, sizeof(g_grants));
    memset(g_audit,   0, sizeof(g_audit));
    memset(g_policy,  0, sizeof(g_policy));
    g_audit_count = 0;
    g_next_token  = 1u;
    g_policy_count = 0;
}

static void cap_allow_policy(const char *grantor, uint32_t kind) {
    if (g_policy_count >= 16u) return;
    strncpy(g_policy[g_policy_count].grantor, grantor, MOCK_AGENT_LEN - 1);
    g_policy[g_policy_count].cap_kind = kind;
    g_policy_count++;
}

static bool cap_policy_ok(const char *grantor, uint32_t kind) {
    for (uint32_t i = 0; i < g_policy_count; i++) {
        if (strcmp(g_policy[i].grantor, grantor) == 0 &&
            g_policy[i].cap_kind == kind)
            return true;
    }
    return false;
}

static void audit_push(uint32_t op, const char *agent_id,
                       uint32_t kind, uint32_t result) {
    if (g_audit_count >= MOCK_MAX_AUDIT) return;
    MockAuditEntry *e = &g_audit[g_audit_count++];
    e->op       = op;
    e->result   = result;
    e->cap_kind = kind;
    strncpy(e->agent_id, agent_id, MOCK_AGENT_LEN - 1);
}

static int cap_find_grant(uint64_t token) {
    for (uint32_t i = 0; i < MOCK_MAX_GRANTS; i++) {
        if (g_grants[i].active && g_grants[i].token == token)
            return (int)i;
    }
    return -1;
}

static int cap_alloc_slot(void) {
    for (uint32_t i = 0; i < MOCK_MAX_GRANTS; i++) {
        if (!g_grants[i].active) return (int)i;
    }
    return -1;
}

/*
 * cap_dispatch — the mock CapabilityBroker IPC handler.
 *
 * MR layout:
 *   MR[0]  = opcode
 *   MR[1+] = op-specific arguments (see per-case comments)
 */
static void cap_dispatch(microkit_channel ch, microkit_msginfo info) {
    (void)ch; (void)info;
    uint64_t op = _mrs[0];

    switch (op) {

    /* ── GRANT ─────────────────────────────────────────────────────────────
     * In:  MR[1] = grantor ptr, MR[2] = grantor len
     *      MR[3] = grantee ptr, MR[4] = grantee len
     *      MR[5] = cap_kind, MR[6] = rights, MR[7] = delegatable (0/1)
     * Out: MR[0] = AOS_OK | AOS_ERR_PERM | AOS_ERR_NOSPC | AOS_ERR_INVAL
     *      MR[1] = token (on success)
     */
    case OP_CAP_GRANT: {
        const char *grantor = (const char *)(uintptr_t)_mrs[1];
        uint32_t    grlen   = (uint32_t)_mrs[2];
        const char *grantee = (const char *)(uintptr_t)_mrs[3];
        uint32_t    geelen  = (uint32_t)_mrs[4];
        uint32_t    kind    = (uint32_t)_mrs[5];
        uint32_t    rights  = (uint32_t)_mrs[6];
        uint32_t    deleg   = (uint32_t)_mrs[7];

        if (!grantor || grlen == 0 || !grantee || geelen == 0) {
            _mrs[0] = AOS_ERR_INVAL;
            break;
        }
        if (!cap_policy_ok(grantor, kind)) {
            audit_push(0, grantor, kind, 1 /* denied */);
            _mrs[0] = AOS_ERR_PERM;
            break;
        }
        int slot = cap_alloc_slot();
        if (slot < 0) {
            _mrs[0] = AOS_ERR_NOSPC;
            break;
        }
        uint64_t tok = g_next_token++;
        MockGrant *g  = &g_grants[slot];
        g->token       = tok;
        g->cap_kind    = kind;
        g->rights      = rights;
        g->delegatable = deleg;
        g->active      = 1u;
        strncpy(g->grantor, grantor, MOCK_AGENT_LEN - 1);
        strncpy(g->grantee, grantee, MOCK_AGENT_LEN - 1);
        audit_push(0, grantor, kind, 0 /* allowed */);
        _mrs[0] = AOS_OK;
        _mrs[1] = tok;
        break;
    }

    /* ── CHECK ─────────────────────────────────────────────────────────────
     * In:  MR[1] = token, MR[2] = required_rights
     * Out: MR[0] = AOS_OK (grant valid + rights match)
     *            | AOS_ERR_NOT_FOUND (unknown token)
     *            | AOS_ERR_PERM (rights insufficient)
     */
    case OP_CAP_CHECK: {
        uint64_t tok      = _mrs[1];
        uint32_t req_rights = (uint32_t)_mrs[2];
        int slot = cap_find_grant(tok);
        if (slot < 0) {
            audit_push(1, "?", 0, 2 /* not_found */);
            _mrs[0] = AOS_ERR_NOT_FOUND;
            break;
        }
        if ((g_grants[slot].rights & req_rights) != req_rights) {
            audit_push(1, g_grants[slot].grantee,
                       g_grants[slot].cap_kind, 1 /* denied */);
            _mrs[0] = AOS_ERR_PERM;
            break;
        }
        audit_push(1, g_grants[slot].grantee,
                   g_grants[slot].cap_kind, 0 /* allowed */);
        _mrs[0] = AOS_OK;
        _mrs[1] = (uint64_t)g_grants[slot].cap_kind;
        break;
    }

    /* ── REVOKE ────────────────────────────────────────────────────────────
     * In:  MR[1] = token
     * Out: MR[0] = AOS_OK | AOS_ERR_NOT_FOUND
     */
    case OP_CAP_REVOKE: {
        uint64_t tok = _mrs[1];
        int slot = cap_find_grant(tok);
        if (slot < 0) {
            _mrs[0] = AOS_ERR_NOT_FOUND;
            break;
        }
        audit_push(2, g_grants[slot].grantee,
                   g_grants[slot].cap_kind, 0 /* allowed */);
        g_grants[slot].active = 0u;
        _mrs[0] = AOS_OK;
        break;
    }

    /* ── QUERY ─────────────────────────────────────────────────────────────
     * In:  MR[1] = grantee ptr, MR[2] = grantee len
     * Out: MR[0] = AOS_OK, MR[1] = count of active grants for this grantee
     */
    case OP_CAP_QUERY: {
        const char *grantee = (const char *)(uintptr_t)_mrs[1];
        uint32_t    geelen  = (uint32_t)_mrs[2];
        if (!grantee || geelen == 0) {
            _mrs[0] = AOS_ERR_INVAL;
            break;
        }
        uint32_t count = 0u;
        for (uint32_t i = 0; i < MOCK_MAX_GRANTS; i++) {
            if (g_grants[i].active &&
                strncmp(g_grants[i].grantee, grantee, MOCK_AGENT_LEN) == 0)
                count++;
        }
        _mrs[0] = AOS_OK;
        _mrs[1] = (uint64_t)count;
        break;
    }

    /* ── AUDIT ─────────────────────────────────────────────────────────────
     * In:  MR[1] = max_entries to return
     * Out: MR[0] = AOS_OK, MR[1] = actual entries available
     */
    case OP_CAP_AUDIT: {
        _mrs[0] = AOS_OK;
        _mrs[1] = (uint64_t)g_audit_count;
        break;
    }

    default:
        _mrs[0] = AOS_ERR_UNIMPL;
        break;
    }
}

/* ── Helper: grant via IPC ───────────────────────────────────────────────── */
static uint64_t do_grant(const char *grantor, const char *grantee,
                         uint32_t kind, uint32_t rights, uint32_t deleg) {
    mock_mr_clear();
    _mrs[0] = OP_CAP_GRANT;
    _mrs[1] = (uint64_t)(uintptr_t)grantor;
    _mrs[2] = (uint64_t)strlen(grantor);
    _mrs[3] = (uint64_t)(uintptr_t)grantee;
    _mrs[4] = (uint64_t)strlen(grantee);
    _mrs[5] = (uint64_t)kind;
    _mrs[6] = (uint64_t)rights;
    _mrs[7] = (uint64_t)deleg;
    cap_dispatch(0, 0);
    return _mrs[0] == AOS_OK ? _mrs[1] : 0;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test functions
 * ════════════════════════════════════════════════════════════════════════════ */

static void test_grant_ok(void) {
    cap_reset();
    cap_allow_policy("kernel", CAP_KIND_MEMORY);
    uint64_t tok = do_grant("kernel", "agent-a", CAP_KIND_MEMORY, CAP_RIGHTS_ALL, 0);
    ASSERT_NE(tok, 0u, "GRANT: valid grant returns non-zero token");
    ASSERT_EQ(_mrs[0], AOS_OK, "GRANT: status is AOS_OK");
}

static void test_grant_policy_denied(void) {
    cap_reset();
    /* No policy entry for "agent-a" issuing MEMORY caps */
    mock_mr_clear();
    _mrs[0] = OP_CAP_GRANT;
    _mrs[1] = (uint64_t)(uintptr_t)"agent-a";
    _mrs[2] = (uint64_t)strlen("agent-a");
    _mrs[3] = (uint64_t)(uintptr_t)"agent-b";
    _mrs[4] = (uint64_t)strlen("agent-b");
    _mrs[5] = CAP_KIND_MEMORY;
    _mrs[6] = CAP_RIGHTS_ALL;
    _mrs[7] = 0;
    cap_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_PERM, "GRANT: policy violation returns AOS_ERR_PERM");
}

static void test_grant_null_grantor(void) {
    cap_reset();
    mock_mr_clear();
    _mrs[0] = OP_CAP_GRANT;
    _mrs[1] = 0;  /* NULL grantor */
    _mrs[2] = 6;
    _mrs[3] = (uint64_t)(uintptr_t)"b";
    _mrs[4] = 1;
    cap_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_INVAL, "GRANT: null grantor returns AOS_ERR_INVAL");
}

static void test_grant_table_full(void) {
    cap_reset();
    cap_allow_policy("root", CAP_KIND_ENDPOINT);
    /* Fill all slots */
    for (uint32_t i = 0; i < MOCK_MAX_GRANTS; i++) {
        do_grant("root", "agent-x", CAP_KIND_ENDPOINT, CAP_RIGHT_READ, 0);
    }
    /* One more should fail */
    mock_mr_clear();
    _mrs[0] = OP_CAP_GRANT;
    _mrs[1] = (uint64_t)(uintptr_t)"root";
    _mrs[2] = 4;
    _mrs[3] = (uint64_t)(uintptr_t)"agent-x";
    _mrs[4] = 7;
    _mrs[5] = CAP_KIND_ENDPOINT;
    _mrs[6] = CAP_RIGHT_READ;
    _mrs[7] = 0;
    cap_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOSPC, "GRANT: full table returns AOS_ERR_NOSPC");
}

static void test_check_valid_grant(void) {
    cap_reset();
    cap_allow_policy("k", CAP_KIND_MEMORY);
    uint64_t tok = do_grant("k", "a", CAP_KIND_MEMORY, CAP_RIGHTS_ALL, 0);

    mock_mr_clear();
    _mrs[0] = OP_CAP_CHECK;
    _mrs[1] = tok;
    _mrs[2] = CAP_RIGHT_READ;
    cap_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "CHECK: valid grant with matching rights returns AOS_OK");
}

static void test_check_insufficient_rights(void) {
    cap_reset();
    cap_allow_policy("k", CAP_KIND_MEMORY);
    /* Grant only READ */
    uint64_t tok = do_grant("k", "a", CAP_KIND_MEMORY, CAP_RIGHT_READ, 0);

    mock_mr_clear();
    _mrs[0] = OP_CAP_CHECK;
    _mrs[1] = tok;
    _mrs[2] = CAP_RIGHT_WRITE;  /* not granted */
    cap_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_PERM,
              "CHECK: insufficient rights returns AOS_ERR_PERM");
}

static void test_check_unknown_token(void) {
    cap_reset();
    mock_mr_clear();
    _mrs[0] = OP_CAP_CHECK;
    _mrs[1] = 0xDEADBEEFu;
    _mrs[2] = CAP_RIGHT_READ;
    cap_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND,
              "CHECK: unknown token returns AOS_ERR_NOT_FOUND");
}

static void test_revoke_ok(void) {
    cap_reset();
    cap_allow_policy("k", CAP_KIND_NOTIFICATION);
    uint64_t tok = do_grant("k", "a", CAP_KIND_NOTIFICATION, CAP_RIGHTS_ALL, 0);

    mock_mr_clear();
    _mrs[0] = OP_CAP_REVOKE;
    _mrs[1] = tok;
    cap_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "REVOKE: valid token returns AOS_OK");
}

static void test_revoke_prevents_check(void) {
    cap_reset();
    cap_allow_policy("k", CAP_KIND_ENDPOINT);
    uint64_t tok = do_grant("k", "a", CAP_KIND_ENDPOINT, CAP_RIGHTS_ALL, 0);

    /* Revoke */
    mock_mr_clear();
    _mrs[0] = OP_CAP_REVOKE;
    _mrs[1] = tok;
    cap_dispatch(0, 0);

    /* Now check — must fail */
    mock_mr_clear();
    _mrs[0] = OP_CAP_CHECK;
    _mrs[1] = tok;
    _mrs[2] = CAP_RIGHT_READ;
    cap_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND,
              "REVOKE: check after revoke returns AOS_ERR_NOT_FOUND");
}

static void test_revoke_unknown_token(void) {
    cap_reset();
    mock_mr_clear();
    _mrs[0] = OP_CAP_REVOKE;
    _mrs[1] = 9999u;
    cap_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND,
              "REVOKE: unknown token returns AOS_ERR_NOT_FOUND");
}

static void test_query_empty(void) {
    cap_reset();
    mock_mr_clear();
    _mrs[0] = OP_CAP_QUERY;
    _mrs[1] = (uint64_t)(uintptr_t)"agent-z";
    _mrs[2] = 7;
    cap_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "QUERY: ok even with no grants");
    ASSERT_EQ(_mrs[1], 0u, "QUERY: count == 0 when agent has no grants");
}

static void test_query_counts_grants(void) {
    cap_reset();
    cap_allow_policy("root", CAP_KIND_MEMORY);
    cap_allow_policy("root", CAP_KIND_ENDPOINT);
    do_grant("root", "bob", CAP_KIND_MEMORY,   CAP_RIGHT_READ, 0);
    do_grant("root", "bob", CAP_KIND_ENDPOINT,  CAP_RIGHTS_ALL, 0);
    do_grant("root", "alice", CAP_KIND_MEMORY,  CAP_RIGHT_READ, 0);

    mock_mr_clear();
    _mrs[0] = OP_CAP_QUERY;
    _mrs[1] = (uint64_t)(uintptr_t)"bob";
    _mrs[2] = 3;
    cap_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "QUERY: status AOS_OK");
    ASSERT_EQ(_mrs[1], 2u, "QUERY: bob has 2 active grants");
}

static void test_query_null_grantee(void) {
    cap_reset();
    mock_mr_clear();
    _mrs[0] = OP_CAP_QUERY;
    _mrs[1] = 0;   /* NULL */
    _mrs[2] = 5;
    cap_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_INVAL, "QUERY: null grantee returns AOS_ERR_INVAL");
}

static void test_audit_empty(void) {
    cap_reset();
    mock_mr_clear();
    _mrs[0] = OP_CAP_AUDIT;
    _mrs[1] = 10;
    cap_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "AUDIT: ok on empty log");
    ASSERT_EQ(_mrs[1], 0u, "AUDIT: 0 entries when no ops performed");
}

static void test_audit_records_grant(void) {
    cap_reset();
    cap_allow_policy("root", CAP_KIND_FRAME);
    do_grant("root", "agent-c", CAP_KIND_FRAME, CAP_RIGHTS_ALL, 0);

    mock_mr_clear();
    _mrs[0] = OP_CAP_AUDIT;
    _mrs[1] = 8;
    cap_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "AUDIT: status AOS_OK after grant");
    ASSERT_EQ(_mrs[1], 1u, "AUDIT: 1 entry recorded after one grant");
}

static void test_audit_records_denied(void) {
    cap_reset();
    /* Policy intentionally absent — grant will be denied */
    mock_mr_clear();
    _mrs[0] = OP_CAP_GRANT;
    _mrs[1] = (uint64_t)(uintptr_t)"rogue";
    _mrs[2] = 5;
    _mrs[3] = (uint64_t)(uintptr_t)"victim";
    _mrs[4] = 6;
    _mrs[5] = CAP_KIND_MEMORY;
    _mrs[6] = CAP_RIGHTS_ALL;
    _mrs[7] = 0;
    cap_dispatch(0, 0);  /* will be denied */

    mock_mr_clear();
    _mrs[0] = OP_CAP_AUDIT;
    _mrs[1] = 8;
    cap_dispatch(0, 0);
    ASSERT_EQ(_mrs[1], 1u,
              "AUDIT: denied grant still produces an audit entry");
}

static void test_unknown_opcode(void) {
    cap_reset();
    mock_mr_clear();
    _mrs[0] = 0xBEu;
    cap_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_UNIMPL,
              "unknown opcode returns AOS_ERR_UNIMPL");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    TAP_PLAN(26);

    test_grant_ok();
    test_grant_policy_denied();
    test_grant_null_grantor();
    test_grant_table_full();

    test_check_valid_grant();
    test_check_insufficient_rights();
    test_check_unknown_token();

    test_revoke_ok();
    test_revoke_prevents_check();
    test_revoke_unknown_token();

    test_query_empty();
    test_query_counts_grants();
    test_query_null_grantee();

    test_audit_empty();
    test_audit_records_grant();
    test_audit_records_denied();

    test_unknown_opcode();

    /* round-trip: grant → check → revoke → check-fails */
    {
        cap_reset();
        cap_allow_policy("root", CAP_KIND_ENDPOINT);
        uint64_t tok = do_grant("root", "svc", CAP_KIND_ENDPOINT,
                                CAP_RIGHT_READ | CAP_RIGHT_WRITE, 0);
        mock_mr_clear();
        _mrs[0] = OP_CAP_CHECK;
        _mrs[1] = tok;
        _mrs[2] = CAP_RIGHT_READ;
        cap_dispatch(0, 0);
        ASSERT_EQ(_mrs[0], AOS_OK, "round-trip: check before revoke passes");

        mock_mr_clear();
        _mrs[0] = OP_CAP_REVOKE;
        _mrs[1] = tok;
        cap_dispatch(0, 0);

        mock_mr_clear();
        _mrs[0] = OP_CAP_CHECK;
        _mrs[1] = tok;
        _mrs[2] = CAP_RIGHT_READ;
        cap_dispatch(0, 0);
        ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND,
                  "round-trip: check after revoke fails");
    }

    /* QUERY count decreases after revoke */
    {
        cap_reset();
        cap_allow_policy("k", CAP_KIND_NOTIFICATION);
        uint64_t tok1 = do_grant("k", "d", CAP_KIND_NOTIFICATION, CAP_RIGHTS_ALL, 0);
        uint64_t tok2 = do_grant("k", "d", CAP_KIND_NOTIFICATION, CAP_RIGHT_READ, 0);

        mock_mr_clear();
        _mrs[0] = OP_CAP_REVOKE; _mrs[1] = tok1;
        cap_dispatch(0, 0);
        (void)tok2;

        mock_mr_clear();
        _mrs[0] = OP_CAP_QUERY;
        _mrs[1] = (uint64_t)(uintptr_t)"d";
        _mrs[2] = 1;
        cap_dispatch(0, 0);
        ASSERT_EQ(_mrs[1], 1u,
                  "QUERY: count drops to 1 after one of two grants is revoked");
    }

    /* AUDIT count increases monotonically */
    {
        cap_reset();
        cap_allow_policy("k", CAP_KIND_MEMORY);
        do_grant("k", "e", CAP_KIND_MEMORY, CAP_RIGHT_READ, 0);
        do_grant("k", "f", CAP_KIND_MEMORY, CAP_RIGHT_WRITE, 0);

        mock_mr_clear();
        _mrs[0] = OP_CAP_AUDIT; _mrs[1] = 16;
        cap_dispatch(0, 0);
        ASSERT_EQ(_mrs[1], 2u, "AUDIT: two grants produce two audit entries");
    }

    return tap_exit();
}

#else
typedef int _agentos_api_test_capstore_dummy;
#endif /* AGENTOS_TEST_HOST */
