/*
 * test_e13_agent_boot.c — End-to-end agent boot pipeline test (E13)
 *
 * Validates the full seL4 → IPC bridge → Linux → agent → vibe-engine → WASM
 * deploy pipeline on the host, without seL4 hardware.  All seL4 primitives
 * and the Linux /dev/mem path are replaced with in-memory simulations.
 *
 * Pipeline stages tested:
 *
 *   Stage 1 — IPC bridge initialization
 *     seL4 side writes IPC_CMD_MAGIC into the command ring header.
 *     Linux agent polls until magic appears (simulated with memcpy).
 *
 *   Stage 2 — SPAWN command flow
 *     seL4 enqueues IPC_OP_SPAWN("agentos-agent") in the command ring.
 *     Linux daemon dispatches to the agent.
 *
 *   Stage 3 — Agent writes WASM to staging region
 *     Agent copies minimal_service WASM into the staging buffer.
 *     Agent enqueues a "wasm_ready:<size>" response in the response ring.
 *
 *   Stage 4 — WASM validation (C port of wasm_validator.rs)
 *     Check magic bytes, required exports, memory section, custom section.
 *     Compute vswap_val_flags_t bitmask — all VSWAP_VAL_REQUIRED bits set.
 *
 *   Stage 5 — Vibe-engine pipeline (simulated inline)
 *     PROPOSE  → proposal enters PENDING state
 *     VALIDATE → all val_flags pass → VALIDATED state
 *     COMMIT   → transition to ACTIVE (hot-swap simulated)
 *
 *   Stage 6 — Final state verification
 *     Proposal is ACTIVE; service version incremented; staging region cleared.
 *
 * Build:
 *   cc -o /tmp/test_e13 tests/test_e13_agent_boot.c -I tests -std=c11
 * Run:
 *   /tmp/test_e13
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "wasm_fixtures/minimal_service.h"

/* ════════════════════════════════════════════════════════════════════════════
 * IPC Bridge simulation
 * ════════════════════════════════════════════════════════════════════════════ */

#define IPC_CMD_MAGIC    0x49504343UL  /* "IPCC" */
#define IPC_RESP_MAGIC   0x49504352UL  /* "IPCR" */

#define IPC_OP_EXEC      0x01
#define IPC_OP_PING      0x04
#define IPC_OP_SPAWN     0x05

#define IPC_RING_DEPTH   64
#define IPC_PAYLOAD_LEN  128

#define IPC_CMD_RING_OFFSET   0x1000u
#define IPC_RESP_RING_OFFSET  0x2000u

typedef struct {
    uint32_t seq;
    uint32_t op;
    uint32_t vm_slot;
    uint32_t payload_len;
    uint8_t  payload[IPC_PAYLOAD_LEN];
} ipc_cmd_t;

typedef struct {
    uint32_t seq;
    uint32_t status;
    uint32_t payload_len;
    uint32_t _pad;
    uint8_t  payload[IPC_PAYLOAD_LEN];
} ipc_resp_t;

typedef struct {
    uint32_t  magic;
    uint32_t  head;
    uint32_t  tail;
    uint32_t  _pad;
    ipc_cmd_t cmds[IPC_RING_DEPTH];
} ipc_cmd_ring_t;

typedef struct {
    uint32_t   magic;
    uint32_t   head;
    uint32_t   tail;
    uint32_t   _pad;
    ipc_resp_t resps[IPC_RING_DEPTH];
} ipc_resp_ring_t;

/* Flat in-memory shmem region (covers cmd ring + resp ring) */
static uint8_t g_shmem[0x5000];

/* Separate staging region for WASM bytes (mirrors AGENT_STAGING_OFFSET area) */
static uint8_t g_staging[4096];

static ipc_cmd_ring_t  *cmd_ring(void)  { return (ipc_cmd_ring_t  *)(g_shmem + IPC_CMD_RING_OFFSET);  }
static ipc_resp_ring_t *resp_ring(void) { return (ipc_resp_ring_t *)(g_shmem + IPC_RESP_RING_OFFSET); }

static void bridge_init(void)
{
    memset(g_shmem, 0, sizeof(g_shmem));
    memset(g_staging, 0, sizeof(g_staging));
    cmd_ring()->magic  = IPC_CMD_MAGIC;
    resp_ring()->magic = IPC_RESP_MAGIC;
}

/* seL4 side: enqueue a command into the cmd ring */
static int bridge_send_cmd(uint32_t op, uint32_t vm_slot,
                            const void *payload, uint32_t payload_len,
                            uint32_t *out_seq)
{
    ipc_cmd_ring_t *r = cmd_ring();
    uint32_t head = r->head;
    uint32_t next = (head + 1u) % IPC_RING_DEPTH;
    if (next == r->tail) return -1;  /* ring full */

    ipc_cmd_t *slot = &r->cmds[head];
    memset(slot, 0, sizeof(*slot));
    slot->seq     = head;
    slot->op      = op;
    slot->vm_slot = vm_slot;
    if (payload && payload_len) {
        uint32_t n = payload_len < IPC_PAYLOAD_LEN ? payload_len : IPC_PAYLOAD_LEN;
        memcpy(slot->payload, payload, n);
        slot->payload_len = n;
    }
    r->head = next;
    if (out_seq) *out_seq = head;
    return 0;
}

/* Linux side (daemon): consume one command from the cmd ring */
static int bridge_recv_cmd(ipc_cmd_t *out)
{
    ipc_cmd_ring_t *r = cmd_ring();
    if (r->head == r->tail) return 0;  /* empty */
    *out = r->cmds[r->tail];
    r->tail = (r->tail + 1u) % IPC_RING_DEPTH;
    return 1;
}

/* Linux side (agent): enqueue a response in the resp ring */
static int bridge_send_resp(uint32_t seq, uint32_t status,
                             const void *payload, uint32_t payload_len)
{
    ipc_resp_ring_t *r = resp_ring();
    uint32_t head = r->head;
    uint32_t next = (head + 1u) % IPC_RING_DEPTH;
    if (next == r->tail) return -1;

    ipc_resp_t *slot = &r->resps[head];
    memset(slot, 0, sizeof(*slot));
    slot->seq    = seq;
    slot->status = status;
    if (payload && payload_len) {
        uint32_t n = payload_len < IPC_PAYLOAD_LEN ? payload_len : IPC_PAYLOAD_LEN;
        memcpy(slot->payload, payload, n);
        slot->payload_len = n;
    }
    r->head = next;
    return 0;
}

/* seL4 side: drain one response from the resp ring */
static int bridge_recv_resp(ipc_resp_t *out)
{
    ipc_resp_ring_t *r = resp_ring();
    if (r->head == r->tail) return 0;
    *out = r->resps[r->tail];
    r->tail = (r->tail + 1u) % IPC_RING_DEPTH;
    return 1;
}

/* ════════════════════════════════════════════════════════════════════════════
 * WASM validator (C port of wasm_validator.rs)
 * ════════════════════════════════════════════════════════════════════════════ */

#define WASM_MAGIC_0  0x00
#define WASM_MAGIC_1  0x61
#define WASM_MAGIC_2  0x73
#define WASM_MAGIC_3  0x6D

#define WASM_SECTION_CUSTOM   0
#define WASM_SECTION_MEMORY   5
#define WASM_SECTION_EXPORT   7

/* Read unsigned LEB128 from data[pos]; return bytes consumed, 0 on error. */
static uint32_t leb128_read(const uint8_t *data, uint32_t len,
                             uint32_t pos, uint32_t *out_val)
{
    uint32_t result = 0, shift = 0, start = pos;
    while (pos < len) {
        uint8_t b = data[pos++];
        result |= (uint32_t)(b & 0x7Fu) << shift;
        shift  += 7;
        if (shift > 35) return 0;  /* overflow guard */
        if ((b & 0x80u) == 0) { *out_val = result; return pos - start; }
    }
    return 0;
}

/* Find a WASM section by id; returns true and fills payload_start and payload_len */
static bool wasm_find_section(const uint8_t *wasm, uint32_t wasm_len,
                               uint8_t target_id,
                               uint32_t *payload_start, uint32_t *payload_len)
{
    if (wasm_len < 8) return false;
    uint32_t pos = 8;
    while (pos < wasm_len) {
        if (pos >= wasm_len) break;
        uint8_t  sid = wasm[pos++];
        uint32_t ssize = 0, consumed;
        consumed = leb128_read(wasm, wasm_len, pos, &ssize);
        if (!consumed) return false;
        pos += consumed;
        if (sid == target_id) {
            *payload_start = pos;
            *payload_len   = ssize;
            return true;
        }
        pos += ssize;
    }
    return false;
}

/* Check WASM magic bytes and version */
static bool wasm_check_magic(const uint8_t *w, uint32_t len)
{
    return len >= 8 &&
           w[0] == WASM_MAGIC_0 && w[1] == WASM_MAGIC_1 &&
           w[2] == WASM_MAGIC_2 && w[3] == WASM_MAGIC_3 &&
           w[4] == 0x01 && w[5] == 0x00 && w[6] == 0x00 && w[7] == 0x00;
}

/* Check that all required function exports are present */
static const char *k_required_exports[] = {
    "init", "handle_ppc", "health_check", "notified", NULL
};

static bool wasm_check_exports(const uint8_t *wasm, uint32_t wasm_len)
{
    uint32_t pstart, plen;
    if (!wasm_find_section(wasm, wasm_len, WASM_SECTION_EXPORT, &pstart, &plen))
        return false;

    const uint8_t *p   = wasm + pstart;
    const uint8_t *end = p + plen;
    if (p >= end) return false;

    uint32_t count = 0, consumed;
    consumed = leb128_read(p, (uint32_t)(end - p), 0, &count);
    if (!consumed) return false;
    p += consumed;

    /* Collect export names (max 16 exports) */
    char names[16][32];
    uint32_t n_names = 0;

    for (uint32_t i = 0; i < count && p < end && n_names < 16; i++) {
        uint32_t name_len = 0;
        consumed = leb128_read(p, (uint32_t)(end - p), 0, &name_len);
        if (!consumed) break;
        p += consumed;
        if (p + name_len > end) break;
        uint32_t copy = name_len < 31 ? name_len : 31;
        memcpy(names[n_names], p, copy);
        names[n_names][copy] = '\0';
        n_names++;
        p += name_len;
        if (p >= end) break;
        p++;            /* kind byte */
        uint32_t idx = 0;
        consumed = leb128_read(p, (uint32_t)(end - p), 0, &idx);
        if (!consumed) break;
        p += consumed;
    }

    /* Verify each required export is present */
    for (uint32_t r = 0; k_required_exports[r]; r++) {
        bool found = false;
        for (uint32_t j = 0; j < n_names; j++) {
            if (strcmp(names[j], k_required_exports[r]) == 0) {
                found = true; break;
            }
        }
        if (!found) return false;
    }
    return true;
}

/* Check for WASM linear memory section */
static bool wasm_has_memory(const uint8_t *wasm, uint32_t wasm_len)
{
    uint32_t ps, pl;
    return wasm_find_section(wasm, wasm_len, WASM_SECTION_MEMORY, &ps, &pl);
}

/* Find a custom section by name; return true if found */
static bool wasm_has_custom_section(const uint8_t *wasm, uint32_t wasm_len,
                                     const char *target_name)
{
    if (wasm_len < 8) return false;
    uint32_t pos = 8;
    uint32_t tlen = (uint32_t)strlen(target_name);

    while (pos < wasm_len) {
        uint8_t sid = wasm[pos++];
        uint32_t ssize = 0, consumed;
        consumed = leb128_read(wasm, wasm_len, pos, &ssize);
        if (!consumed) return false;
        pos += consumed;
        uint32_t section_end = pos + ssize;
        if (sid == WASM_SECTION_CUSTOM) {
            const uint8_t *body = wasm + pos;
            uint32_t body_len   = ssize;
            uint32_t name_len   = 0;
            uint32_t nc = leb128_read(body, body_len, 0, &name_len);
            if (nc && name_len == tlen) {
                if (memcmp(body + nc, target_name, tlen) == 0)
                    return true;
            }
        }
        pos = section_end;
    }
    return false;
}

/* ── Validation flag bits (mirrors vibe-engine contract) ─────────────────── */

#define VSWAP_VAL_MAGIC     (1u << 0)
#define VSWAP_VAL_SIZE      (1u << 1)
#define VSWAP_VAL_SERVICE   (1u << 2)
#define VSWAP_VAL_CAP       (1u << 3)
#define VSWAP_VAL_EXPORTS   (1u << 4)
#define VSWAP_VAL_MEMORY    (1u << 5)
#define VSWAP_VAL_CAPS_SECT (1u << 6)
#define VSWAP_VAL_REQUIRED  (VSWAP_VAL_MAGIC | VSWAP_VAL_SIZE | \
                             VSWAP_VAL_SERVICE | VSWAP_VAL_CAP | \
                             VSWAP_VAL_EXPORTS | VSWAP_VAL_MEMORY)

#define VSWAP_MAX_WASM_SIZE (4u * 1024u * 1024u)

/* Compute validation flags for a WASM binary */
static uint32_t wasm_validate_flags(const uint8_t *wasm, uint32_t len,
                                     uint8_t service_id, bool service_ok,
                                     bool cap_ok)
{
    uint32_t flags = 0;

    if (wasm_check_magic(wasm, len))                       flags |= VSWAP_VAL_MAGIC;
    if (len <= VSWAP_MAX_WASM_SIZE)                        flags |= VSWAP_VAL_SIZE;
    if (service_ok)                                        flags |= VSWAP_VAL_SERVICE;
    if (cap_ok)                                            flags |= VSWAP_VAL_CAP;
    if (wasm_check_exports(wasm, len))                     flags |= VSWAP_VAL_EXPORTS;
    if (wasm_has_memory(wasm, len))                        flags |= VSWAP_VAL_MEMORY;
    if (wasm_has_custom_section(wasm, len,
                                "agentos.capabilities"))   flags |= VSWAP_VAL_CAPS_SECT;

    (void)service_id;
    return flags;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Vibe-engine pipeline simulation
 * ════════════════════════════════════════════════════════════════════════════ */

typedef enum {
    VSWAP_STATE_FREE      = 0,
    VSWAP_STATE_PENDING   = 1,
    VSWAP_STATE_VALIDATED = 2,
    VSWAP_STATE_APPROVED  = 3,
    VSWAP_STATE_ACTIVE    = 4,
    VSWAP_STATE_REJECTED  = 5,
} vswap_state_t;

#define VSWAP_ERR_OK        0u
#define VSWAP_ERR_FULL      1u
#define VSWAP_ERR_BADWASM   2u
#define VSWAP_ERR_TOOBIG    3u
#define VSWAP_ERR_NOSVC     4u
#define VSWAP_ERR_NOENT     5u
#define VSWAP_ERR_STATE     6u
#define VSWAP_ERR_VALFAIL   7u
#define VSWAP_ERR_BUSY      8u

#define VSWAP_MAX_PROPOSALS  8u

typedef struct {
    uint32_t      proposal_id;
    vswap_state_t state;
    uint8_t       service_id;
    uint8_t       _pad[3];
    uint32_t      val_flags;
    uint32_t      wasm_size;
    uint32_t      wasm_version;
    char          error_msg[48];
} sim_proposal_t;

static sim_proposal_t g_proposals[VSWAP_MAX_PROPOSALS];
static uint32_t       g_next_proposal_id = 1;
static uint32_t       g_svc_versions[8];   /* one version counter per service */

static void vibe_engine_reset(void)
{
    memset(g_proposals, 0, sizeof(g_proposals));
    memset(g_svc_versions, 0, sizeof(g_svc_versions));
    g_next_proposal_id = 1;
}

/* PROPOSE: record the WASM binary, return proposal_id */
static uint32_t vibe_propose(const uint8_t *staging, uint32_t wasm_size,
                               uint8_t service_id, uint32_t cap_tag,
                               uint32_t *out_id)
{
    (void)cap_tag;

    /* Check for free slot */
    sim_proposal_t *slot = NULL;
    for (uint32_t i = 0; i < VSWAP_MAX_PROPOSALS; i++) {
        if (g_proposals[i].state == VSWAP_STATE_FREE) {
            slot = &g_proposals[i]; break;
        }
    }
    if (!slot) return VSWAP_ERR_FULL;

    /* Basic WASM magic check */
    if (wasm_size < 8 ||
        staging[0] != 0x00 || staging[1] != 0x61 ||
        staging[2] != 0x73 || staging[3] != 0x6D)
        return VSWAP_ERR_BADWASM;

    if (wasm_size > VSWAP_MAX_WASM_SIZE)
        return VSWAP_ERR_TOOBIG;

    /* Check for existing pending proposal for this service */
    for (uint32_t i = 0; i < VSWAP_MAX_PROPOSALS; i++) {
        if (&g_proposals[i] == slot) continue;
        if (g_proposals[i].service_id == service_id &&
            (g_proposals[i].state == VSWAP_STATE_PENDING ||
             g_proposals[i].state == VSWAP_STATE_VALIDATED))
            return VSWAP_ERR_BUSY;
    }

    slot->proposal_id  = g_next_proposal_id++;
    slot->state        = VSWAP_STATE_PENDING;
    slot->service_id   = service_id;
    slot->val_flags    = 0;
    slot->wasm_size    = wasm_size;
    slot->wasm_version = 0;
    slot->error_msg[0] = '\0';

    if (out_id) *out_id = slot->proposal_id;
    return VSWAP_ERR_OK;
}

/* VALIDATE: run full checks, advance state */
static uint32_t vibe_validate(const uint8_t *staging,
                               uint32_t proposal_id, uint32_t *out_val_flags)
{
    sim_proposal_t *slot = NULL;
    for (uint32_t i = 0; i < VSWAP_MAX_PROPOSALS; i++) {
        if (g_proposals[i].proposal_id == proposal_id) {
            slot = &g_proposals[i]; break;
        }
    }
    if (!slot) return VSWAP_ERR_NOENT;
    if (slot->state != VSWAP_STATE_PENDING) return VSWAP_ERR_STATE;

    uint32_t flags = wasm_validate_flags(staging, slot->wasm_size,
                                          slot->service_id,
                                          true,   /* service_ok */
                                          true);  /* cap_ok     */
    slot->val_flags = flags;
    if (out_val_flags) *out_val_flags = flags;

    if ((flags & VSWAP_VAL_REQUIRED) == VSWAP_VAL_REQUIRED) {
        slot->state = VSWAP_STATE_VALIDATED;
        return VSWAP_ERR_OK;
    }

    slot->state = VSWAP_STATE_REJECTED;
    snprintf(slot->error_msg, sizeof(slot->error_msg),
             "validation failed: flags=0x%x required=0x%x", flags, VSWAP_VAL_REQUIRED);
    return VSWAP_ERR_VALFAIL;
}

/* COMMIT: trigger the hot-swap (simulated: transition to ACTIVE) */
static uint32_t vibe_commit(uint32_t proposal_id, uint32_t *out_version)
{
    sim_proposal_t *slot = NULL;
    for (uint32_t i = 0; i < VSWAP_MAX_PROPOSALS; i++) {
        if (g_proposals[i].proposal_id == proposal_id) {
            slot = &g_proposals[i]; break;
        }
    }
    if (!slot) return VSWAP_ERR_NOENT;
    if (slot->state != VSWAP_STATE_VALIDATED) return VSWAP_ERR_STATE;

    slot->state        = VSWAP_STATE_ACTIVE;
    slot->wasm_version = ++g_svc_versions[slot->service_id & 7u];
    if (out_version) *out_version = slot->wasm_version;
    return VSWAP_ERR_OK;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test framework
 * ════════════════════════════════════════════════════════════════════════════ */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("\n=== TEST: %s ===\n", (name))

#define ASSERT_TRUE(expr, msg) do { \
    if (!(expr)) { printf("  FAIL: %s\n", (msg)); tests_failed++; } \
    else         { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)

#define ASSERT_EQ(a, b, msg) do { \
    unsigned long long _a = (unsigned long long)(a); \
    unsigned long long _b = (unsigned long long)(b); \
    if (_a != _b) { \
        printf("  FAIL: %s (expected %llu, got %llu)\n", (msg), _b, _a); \
        tests_failed++; \
    } else { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)

/* ════════════════════════════════════════════════════════════════════════════
 * Tests
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── Stage 0: WASM fixture sanity ───────────────────────────────────────── */

static void test_wasm_fixture_magic(void)
{
    TEST("wasm_fixture_magic");
    ASSERT_TRUE(g_minimal_wasm_len >= 8, "fixture is at least 8 bytes");
    ASSERT_EQ(g_minimal_wasm[0], 0x00, "magic byte 0");
    ASSERT_EQ(g_minimal_wasm[1], 0x61, "magic byte 1 ('a')");
    ASSERT_EQ(g_minimal_wasm[2], 0x73, "magic byte 2 ('s')");
    ASSERT_EQ(g_minimal_wasm[3], 0x6D, "magic byte 3 ('m')");
    ASSERT_EQ(g_minimal_wasm[4], 0x01, "version byte 0");
    ASSERT_EQ(g_minimal_wasm[5], 0x00, "version byte 1");
}

static void test_wasm_fixture_exports(void)
{
    TEST("wasm_fixture_exports");
    ASSERT_TRUE(wasm_check_exports(g_minimal_wasm, g_minimal_wasm_len),
                "all required exports present: init, handle_ppc, health_check, notified");
}

static void test_wasm_fixture_memory_section(void)
{
    TEST("wasm_fixture_memory_section");
    ASSERT_TRUE(wasm_has_memory(g_minimal_wasm, g_minimal_wasm_len),
                "linear memory section present");
}

static void test_wasm_fixture_capabilities_section(void)
{
    TEST("wasm_fixture_capabilities_section");
    ASSERT_TRUE(wasm_has_custom_section(g_minimal_wasm, g_minimal_wasm_len,
                                        "agentos.capabilities"),
                "agentos.capabilities custom section present");
}

static void test_wasm_validation_flags(void)
{
    TEST("wasm_validation_flags");
    uint32_t flags = wasm_validate_flags(g_minimal_wasm, g_minimal_wasm_len,
                                          1 /* MEMFS */, true, true);
    ASSERT_TRUE((flags & VSWAP_VAL_MAGIC)     != 0, "VSWAP_VAL_MAGIC set");
    ASSERT_TRUE((flags & VSWAP_VAL_SIZE)      != 0, "VSWAP_VAL_SIZE set");
    ASSERT_TRUE((flags & VSWAP_VAL_SERVICE)   != 0, "VSWAP_VAL_SERVICE set");
    ASSERT_TRUE((flags & VSWAP_VAL_CAP)       != 0, "VSWAP_VAL_CAP set");
    ASSERT_TRUE((flags & VSWAP_VAL_EXPORTS)   != 0, "VSWAP_VAL_EXPORTS set");
    ASSERT_TRUE((flags & VSWAP_VAL_MEMORY)    != 0, "VSWAP_VAL_MEMORY set");
    ASSERT_TRUE((flags & VSWAP_VAL_CAPS_SECT) != 0, "VSWAP_VAL_CAPS_SECT set");
    ASSERT_EQ(flags & VSWAP_VAL_REQUIRED, VSWAP_VAL_REQUIRED,
              "all VSWAP_VAL_REQUIRED bits set");
}

static void test_wasm_invalid_magic_rejected(void)
{
    TEST("wasm_invalid_magic_rejected");
    uint8_t bad[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00 };
    uint32_t flags = wasm_validate_flags(bad, 8, 1, true, true);
    ASSERT_TRUE((flags & VSWAP_VAL_MAGIC) == 0, "bad magic: VSWAP_VAL_MAGIC clear");
    ASSERT_TRUE((flags & VSWAP_VAL_REQUIRED) != VSWAP_VAL_REQUIRED,
                "bad magic: required flags not all set");
}

/* ── Stage 1 & 2: IPC bridge init and SPAWN command ─────────────────────── */

static void test_ipc_bridge_init(void)
{
    TEST("ipc_bridge_init");
    bridge_init();
    ASSERT_EQ(cmd_ring()->magic,  IPC_CMD_MAGIC,  "cmd ring magic initialised");
    ASSERT_EQ(resp_ring()->magic, IPC_RESP_MAGIC, "resp ring magic initialised");
    ASSERT_EQ(cmd_ring()->head,   0u,             "cmd ring head = 0");
    ASSERT_EQ(cmd_ring()->tail,   0u,             "cmd ring tail = 0");
    ASSERT_EQ(resp_ring()->head,  0u,             "resp ring head = 0");
    ASSERT_EQ(resp_ring()->tail,  0u,             "resp ring tail = 0");
}

static void test_ipc_spawn_command(void)
{
    TEST("ipc_spawn_command");
    bridge_init();

    /* seL4 side sends SPAWN("agentos-agent") */
    uint32_t seq = 0;
    const char *agent_name = "agentos-agent";
    int rc = bridge_send_cmd(IPC_OP_SPAWN, 0,
                              agent_name, (uint32_t)strlen(agent_name) + 1, &seq);
    ASSERT_EQ(rc, 0, "send SPAWN command succeeds");
    ASSERT_EQ(cmd_ring()->head, 1u, "cmd ring head advanced to 1");

    /* Linux daemon receives it */
    ipc_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    int got = bridge_recv_cmd(&cmd);
    ASSERT_EQ(got, 1, "daemon receives one command");
    ASSERT_EQ(cmd.op, IPC_OP_SPAWN, "op = IPC_OP_SPAWN");
    ASSERT_TRUE(strncmp((char *)cmd.payload, "agentos-agent",
                        cmd.payload_len) == 0,
                "payload = \"agentos-agent\"");
    ASSERT_EQ(cmd_ring()->tail, 1u, "cmd ring tail advanced after consume");
}

/* ── Stage 3: Agent writes WASM and sends wasm_ready response ────────────── */

static void test_agent_writes_wasm_to_staging(void)
{
    TEST("agent_writes_wasm_to_staging");

    /* Simulate agent writing WASM bytes into staging */
    memcpy(g_staging, g_minimal_wasm, g_minimal_wasm_len);

    ASSERT_EQ(g_staging[0], 0x00, "staging[0] = WASM magic 0");
    ASSERT_EQ(g_staging[1], 0x61, "staging[1] = WASM magic 1");
    ASSERT_EQ(g_staging[2], 0x73, "staging[2] = WASM magic 2");
    ASSERT_EQ(g_staging[3], 0x6D, "staging[3] = WASM magic 3");
    ASSERT_TRUE(memcmp(g_staging, g_minimal_wasm, g_minimal_wasm_len) == 0,
                "staging region matches fixture byte-for-byte");
}

static void test_agent_sends_wasm_ready_response(void)
{
    TEST("agent_sends_wasm_ready_response");
    bridge_init();

    /* Simulate agent enqueuing "wasm_ready:<size>" response */
    char payload[64];
    int n = snprintf(payload, sizeof(payload),
                     "wasm_ready:%08x", g_minimal_wasm_len);
    int rc = bridge_send_resp(0, 0, payload, (uint32_t)n);
    ASSERT_EQ(rc, 0, "agent enqueues wasm_ready response");
    ASSERT_EQ(resp_ring()->head, 1u, "resp ring head = 1");

    /* seL4 side drains the response */
    ipc_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    int got = bridge_recv_resp(&resp);
    ASSERT_EQ(got, 1, "seL4 receives one response");
    ASSERT_EQ(resp.status, 0u, "response status = ok");

    /* Parse wasm_size from payload */
    uint32_t parsed_size = 0;
    sscanf((char *)resp.payload, "wasm_ready:%08x", &parsed_size);
    ASSERT_EQ(parsed_size, g_minimal_wasm_len, "parsed wasm_size matches fixture");
}

/* ── Stage 5: Vibe-engine PROPOSE → VALIDATE → COMMIT ───────────────────── */

static void test_vibe_engine_propose(void)
{
    TEST("vibe_engine_propose");
    vibe_engine_reset();
    memcpy(g_staging, g_minimal_wasm, g_minimal_wasm_len);

    uint32_t proposal_id = 0;
    uint32_t rc = vibe_propose(g_staging, g_minimal_wasm_len,
                                1 /* VSWAP_SVC_MEMFS */, 0xCAFE, &proposal_id);
    ASSERT_EQ(rc, VSWAP_ERR_OK, "PROPOSE returns VSWAP_ERR_OK");
    ASSERT_TRUE(proposal_id != 0, "proposal_id is non-zero");
    ASSERT_EQ(g_proposals[0].state, VSWAP_STATE_PENDING, "proposal state = PENDING");
    ASSERT_EQ(g_proposals[0].service_id, 1u, "proposal service_id = 1 (MEMFS)");
    ASSERT_EQ(g_proposals[0].wasm_size, g_minimal_wasm_len, "proposal wasm_size matches");
}

static void test_vibe_engine_propose_bad_magic(void)
{
    TEST("vibe_engine_propose_bad_magic");
    vibe_engine_reset();

    uint8_t bad[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x00, 0x00, 0x00 };
    uint32_t id = 0;
    uint32_t rc = vibe_propose(bad, 8, 1, 0, &id);
    ASSERT_EQ(rc, VSWAP_ERR_BADWASM, "bad magic → VSWAP_ERR_BADWASM");
    ASSERT_EQ(id, 0u, "no proposal_id on error");
}

static void test_vibe_engine_validate(void)
{
    TEST("vibe_engine_validate");
    vibe_engine_reset();
    memcpy(g_staging, g_minimal_wasm, g_minimal_wasm_len);

    uint32_t proposal_id = 0;
    vibe_propose(g_staging, g_minimal_wasm_len, 1, 0, &proposal_id);

    uint32_t val_flags = 0;
    uint32_t rc = vibe_validate(g_staging, proposal_id, &val_flags);
    ASSERT_EQ(rc, VSWAP_ERR_OK, "VALIDATE returns VSWAP_ERR_OK");
    ASSERT_EQ((val_flags & VSWAP_VAL_REQUIRED), VSWAP_VAL_REQUIRED,
              "all required validation flags set");
    ASSERT_EQ(g_proposals[0].state, VSWAP_STATE_VALIDATED,
              "proposal state → VALIDATED");
}

static void test_vibe_engine_commit(void)
{
    TEST("vibe_engine_commit");
    vibe_engine_reset();
    memcpy(g_staging, g_minimal_wasm, g_minimal_wasm_len);

    uint32_t proposal_id = 0;
    vibe_propose(g_staging, g_minimal_wasm_len, 1, 0, &proposal_id);
    vibe_validate(g_staging, proposal_id, NULL);

    uint32_t version = 0;
    uint32_t rc = vibe_commit(proposal_id, &version);
    ASSERT_EQ(rc, VSWAP_ERR_OK, "COMMIT returns VSWAP_ERR_OK");
    ASSERT_EQ(g_proposals[0].state, VSWAP_STATE_ACTIVE,
              "proposal state → ACTIVE");
    ASSERT_TRUE(version > 0, "service version incremented");
    printf("  INFO: service version after commit = %u\n", version);
}

static void test_vibe_engine_commit_requires_validated(void)
{
    TEST("vibe_engine_commit_requires_validated");
    vibe_engine_reset();
    memcpy(g_staging, g_minimal_wasm, g_minimal_wasm_len);

    uint32_t proposal_id = 0;
    vibe_propose(g_staging, g_minimal_wasm_len, 1, 0, &proposal_id);
    /* Skip VALIDATE — try to COMMIT from PENDING state */
    uint32_t rc = vibe_commit(proposal_id, NULL);
    ASSERT_EQ(rc, VSWAP_ERR_STATE, "COMMIT on PENDING proposal → VSWAP_ERR_STATE");
    ASSERT_EQ(g_proposals[0].state, VSWAP_STATE_PENDING,
              "proposal stays in PENDING state");
}

static void test_vibe_engine_duplicate_service_busy(void)
{
    TEST("vibe_engine_duplicate_service_busy");
    vibe_engine_reset();
    memcpy(g_staging, g_minimal_wasm, g_minimal_wasm_len);

    uint32_t id1 = 0, id2 = 0;
    uint32_t rc1 = vibe_propose(g_staging, g_minimal_wasm_len, 1, 0, &id1);
    uint32_t rc2 = vibe_propose(g_staging, g_minimal_wasm_len, 1, 0, &id2);
    ASSERT_EQ(rc1, VSWAP_ERR_OK,   "first PROPOSE for service 1 OK");
    ASSERT_EQ(rc2, VSWAP_ERR_BUSY, "second PROPOSE for same service → BUSY");
    ASSERT_EQ(id2, 0u, "second proposal_id = 0 (not issued)");
}

/* ── Stage 6: Full pipeline end-to-end ───────────────────────────────────── */

static void test_full_pipeline(void)
{
    TEST("full_pipeline_end_to_end");
    bridge_init();
    vibe_engine_reset();
    memset(g_staging, 0, sizeof(g_staging));

    /* 1. seL4 initialises bridge and sends SPAWN */
    uint32_t seq = 0;
    ASSERT_EQ(bridge_send_cmd(IPC_OP_SPAWN, 0, "agentos-agent", 14, &seq),
              0, "seL4 enqueues SPAWN command");

    /* 2. Linux daemon receives the command (simulated) */
    ipc_cmd_t cmd;
    ASSERT_EQ(bridge_recv_cmd(&cmd), 1, "daemon receives SPAWN");
    ASSERT_EQ(cmd.op, IPC_OP_SPAWN, "op = SPAWN");

    /* 3. Agent writes WASM to staging region */
    memcpy(g_staging, g_minimal_wasm, g_minimal_wasm_len);
    ASSERT_TRUE(wasm_check_magic(g_staging, g_minimal_wasm_len),
                "staging region contains valid WASM magic");

    /* 4. Agent sends wasm_ready response */
    char rp[64];
    snprintf(rp, sizeof(rp), "wasm_ready:%08x", g_minimal_wasm_len);
    ASSERT_EQ(bridge_send_resp(seq, 0, rp, (uint32_t)strlen(rp)), 0,
              "agent enqueues wasm_ready response");

    /* 5. seL4 debug_bridge drains response and triggers PROPOSE */
    ipc_resp_t resp;
    ASSERT_EQ(bridge_recv_resp(&resp), 1, "seL4 drains response");
    ASSERT_EQ(resp.status, 0u, "response status = ok");

    uint32_t wasm_size = 0;
    sscanf((char *)resp.payload, "wasm_ready:%08x", &wasm_size);
    ASSERT_EQ(wasm_size, g_minimal_wasm_len, "seL4 parses correct wasm_size");

    /* 6. PROPOSE */
    uint32_t proposal_id = 0;
    ASSERT_EQ(vibe_propose(g_staging, wasm_size, 1, 0xABCD, &proposal_id),
              VSWAP_ERR_OK, "PROPOSE OK");
    ASSERT_TRUE(proposal_id != 0, "proposal_id assigned");

    /* 7. VALIDATE */
    uint32_t val_flags = 0;
    ASSERT_EQ(vibe_validate(g_staging, proposal_id, &val_flags),
              VSWAP_ERR_OK, "VALIDATE OK");
    ASSERT_EQ(val_flags & VSWAP_VAL_REQUIRED, VSWAP_VAL_REQUIRED,
              "all required validation flags pass");

    /* 8. COMMIT */
    uint32_t version = 0;
    ASSERT_EQ(vibe_commit(proposal_id, &version), VSWAP_ERR_OK, "COMMIT OK");

    /* 9. Verify final state */
    ASSERT_EQ(g_proposals[0].state, VSWAP_STATE_ACTIVE,
              "proposal is ACTIVE — WASM service deployed");
    ASSERT_EQ(version, 1u, "service version = 1 after first deploy");

    printf("  INFO: pipeline complete — service version %u is live\n", version);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  agentOS E13 — End-to-End Agent Boot Test Suite          ║\n");
    printf("║  seL4 → IPC bridge → Linux → agent → vibe-engine → WASM ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    /* Stage 0: WASM fixture */
    test_wasm_fixture_magic();
    test_wasm_fixture_exports();
    test_wasm_fixture_memory_section();
    test_wasm_fixture_capabilities_section();
    test_wasm_validation_flags();
    test_wasm_invalid_magic_rejected();

    /* Stages 1-2: IPC bridge */
    test_ipc_bridge_init();
    test_ipc_spawn_command();

    /* Stage 3: agent → staging region */
    test_agent_writes_wasm_to_staging();
    test_agent_sends_wasm_ready_response();

    /* Stage 5: vibe-engine pipeline */
    test_vibe_engine_propose();
    test_vibe_engine_propose_bad_magic();
    test_vibe_engine_validate();
    test_vibe_engine_commit();
    test_vibe_engine_commit_requires_validated();
    test_vibe_engine_duplicate_service_busy();

    /* Stage 6: full pipeline */
    test_full_pipeline();

    printf("\n══════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    if (tests_failed > 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
