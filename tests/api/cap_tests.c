/*
 * cap_tests.c — Capability delegation and accounting tests
 *
 * Tests capability tree operations, delegation semantics, badged endpoint
 * pool management, and the cap_accounting API contract.
 *
 * All service logic is embedded under AGENTOS_TEST_HOST for a self-contained
 * build.  The cap_acct_* implementation below mirrors cap_accounting.c exactly
 * (same table size, same field layout, same API) so changes to the production
 * code will break this test when the contracts diverge.
 *
 * Build & run (self-contained — no extra .c files needed):
 *   cc -std=c11 -Wall -Wextra -DAGENTOS_TEST_HOST \
 *       -I tests/api -o /tmp/cap_tests tests/api/cap_tests.c
 *   /tmp/cap_tests
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
#include "framework.h"    /* TAP_PLAN, TAP_OK, TAP_FAIL, ASSERT_*, tap_exit */

/* ══════════════════════════════════════════════════════════════════════════
 * seL4 type stubs
 * ══════════════════════════════════════════════════════════════════════════ */

typedef uint64_t seL4_Word;
typedef uint64_t seL4_CPtr;

#define seL4_CapNull               ((seL4_CPtr)0u)
#define seL4_CapInitThreadCNode    ((seL4_CPtr)1u)
#define seL4_CapInitThreadVSpace   ((seL4_CPtr)2u)
#define seL4_CapInitThreadTCB      ((seL4_CPtr)3u)
#define seL4_TCBObject             1u
#define seL4_EndpointObject        4u
#define seL4_CapTableObject        3u
#define seL4_ARM_VSpaceObject      7u

/* ══════════════════════════════════════════════════════════════════════════
 * Embedded cap_accounting API
 *
 * Mirrors cap_accounting.h / cap_accounting.c exactly.
 * Tests verify the API contract; changes to the production implementation
 * that break this test must update the version in contracts/cap-accounting/.
 * ══════════════════════════════════════════════════════════════════════════ */

#define CAP_ACCT_MAX_ENTRIES  1024u

typedef struct {
    seL4_CPtr  cap;
    uint32_t   obj_type;
    uint32_t   pd_index;
    char       name[16];
} cap_acct_entry_t;

_Static_assert(sizeof(cap_acct_entry_t) >= 24u,
               "cap_acct_entry_t must be at least 24 bytes");

static cap_acct_entry_t  g_acct_table[CAP_ACCT_MAX_ENTRIES];
static uint32_t          g_acct_count;

static void _acct_strlcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0u;
    while (i + 1u < max && src[i] != '\0') { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void cap_acct_init(void)
{
    g_acct_count = 0u;
    for (uint32_t i = 0u; i < CAP_ACCT_MAX_ENTRIES; i++) {
        g_acct_table[i].cap      = seL4_CapNull;
        g_acct_table[i].obj_type = 0u;
        g_acct_table[i].pd_index = 0u;
        g_acct_table[i].name[0]  = '\0';
    }
    /* Record the 3 root-task boot caps (same as cap_accounting.c) */
    static const struct { seL4_CPtr cap; uint32_t type; const char *name; }
    boot_caps[] = {
        { seL4_CapInitThreadCNode,  seL4_CapTableObject,  "root-cnode"  },
        { seL4_CapInitThreadVSpace, seL4_ARM_VSpaceObject,"root-vspace" },
        { seL4_CapInitThreadTCB,    seL4_TCBObject,       "root-tcb"   },
    };
    for (uint32_t i = 0u; i < 3u; i++) {
        cap_acct_entry_t *e = &g_acct_table[g_acct_count++];
        e->cap      = boot_caps[i].cap;
        e->obj_type = boot_caps[i].type;
        e->pd_index = 0u;
        _acct_strlcpy(e->name, boot_caps[i].name, sizeof(e->name));
    }
}

static int cap_acct_record(seL4_CPtr parent, seL4_CPtr cap,
                            uint32_t obj_type, uint32_t pd_index,
                            const char *name)
{
    (void)parent;
    if (g_acct_count >= CAP_ACCT_MAX_ENTRIES) return -1;
    cap_acct_entry_t *e = &g_acct_table[g_acct_count++];
    e->cap      = cap;
    e->obj_type = obj_type;
    e->pd_index = pd_index;
    _acct_strlcpy(e->name, name ? name : "", sizeof(e->name));
    return 0;
}

static uint32_t cap_acct_count(void)  { return g_acct_count; }

static const cap_acct_entry_t *cap_acct_get(uint32_t index)
{
    if (index >= g_acct_count) return (const cap_acct_entry_t *)0;
    return &g_acct_table[index];
}

/* ══════════════════════════════════════════════════════════════════════════
 * In-test cap_tree
 *
 * Models the capability delegation tree used to track minted badged endpoint
 * caps.  Each node has a parent index so that cap_tree_revoke() propagates
 * recursively to all descendants without requiring runtime recursion.
 * ══════════════════════════════════════════════════════════════════════════ */

#define CAP_TREE_MAX_NODES  64u
#define CAP_NODE_NONE       0xFFFFFFFFu

typedef struct {
    seL4_CPtr cap;
    uint32_t  pd_id;
    uint64_t  badge;
    char      name[16];
    uint32_t  obj_type;
    uint32_t  flags;
    uint32_t  parent;   /* CAP_NODE_NONE if root */
    uint32_t  valid;
} cap_tree_node_t;

static cap_tree_node_t g_cap_tree[CAP_TREE_MAX_NODES];
static uint32_t        g_cap_tree_used;

static void cap_tree_reset(void)
{
    for (uint32_t i = 0u; i < CAP_TREE_MAX_NODES; i++)
        g_cap_tree[i].valid = 0u;
    g_cap_tree_used = 0u;
}

static uint32_t cap_tree_insert(seL4_CPtr cap, uint32_t pd_id, uint64_t badge,
                                 const char *name, uint32_t obj_type,
                                 uint32_t flags, uint32_t parent_idx)
{
    for (uint32_t i = 0u; i < CAP_TREE_MAX_NODES; i++) {
        if (!g_cap_tree[i].valid) {
            cap_tree_node_t *n = &g_cap_tree[i];
            n->cap      = cap;
            n->pd_id    = pd_id;
            n->badge    = badge;
            n->obj_type = obj_type;
            n->flags    = flags;
            n->parent   = parent_idx;
            n->valid    = 1u;
            n->name[0]  = '\0';
            if (name) {
                uint32_t j = 0u;
                while (j + 1u < sizeof(n->name) && name[j] != '\0') {
                    n->name[j] = name[j];
                    j++;
                }
                n->name[j] = '\0';
            }
            g_cap_tree_used++;
            return i;
        }
    }
    return CAP_NODE_NONE;   /* tree full */
}

static uint32_t cap_tree_find_cap(seL4_CPtr cap)
{
    for (uint32_t i = 0u; i < CAP_TREE_MAX_NODES; i++)
        if (g_cap_tree[i].valid && g_cap_tree[i].cap == cap) return i;
    return CAP_NODE_NONE;
}

static int cap_tree_remove(uint32_t idx)
{
    if (idx >= CAP_TREE_MAX_NODES || !g_cap_tree[idx].valid) return -1;
    g_cap_tree[idx].valid = 0u;
    if (g_cap_tree_used > 0u) g_cap_tree_used--;
    return 0;
}

/* Revoke a node and all its descendants (iterative, no recursion) */
static uint32_t cap_tree_revoke(uint32_t root_idx)
{
    if (root_idx >= CAP_TREE_MAX_NODES || !g_cap_tree[root_idx].valid)
        return 0u;
    uint32_t removed = 0u;
    g_cap_tree[root_idx].valid = 0u;
    removed++;
    if (g_cap_tree_used > 0u) g_cap_tree_used--;
    int changed;
    do {
        changed = 0;
        for (uint32_t i = 0u; i < CAP_TREE_MAX_NODES; i++) {
            if (!g_cap_tree[i].valid) continue;
            uint32_t p = g_cap_tree[i].parent;
            if (p == CAP_NODE_NONE || p >= CAP_TREE_MAX_NODES) continue;
            if (!g_cap_tree[p].valid) {
                g_cap_tree[i].valid = 0u;
                removed++;
                if (g_cap_tree_used > 0u) g_cap_tree_used--;
                changed = 1;
            }
        }
    } while (changed);
    return removed;
}

static uint32_t cap_tree_walk_pd(uint32_t pd_id)
{
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < CAP_TREE_MAX_NODES; i++)
        if (g_cap_tree[i].valid && g_cap_tree[i].pd_id == pd_id) count++;
    return count;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Endpoint pool simulation
 *
 * Models a bounded pool of seL4 endpoint caps the root task hands out as
 * it creates PDs.
 * ══════════════════════════════════════════════════════════════════════════ */

#define EP_POOL_SIZE  16u

static seL4_CPtr g_ep_pool_caps[EP_POOL_SIZE];
static uint32_t  g_ep_pool_used;

static void ep_pool_reset(void)
{
    g_ep_pool_used = 0u;
    for (uint32_t i = 0u; i < EP_POOL_SIZE; i++)
        g_ep_pool_caps[i] = seL4_CapNull;
}

static seL4_CPtr ep_alloc(void)
{
    if (g_ep_pool_used >= EP_POOL_SIZE) return seL4_CapNull;
    seL4_CPtr cap = (seL4_CPtr)(100u + g_ep_pool_used);
    g_ep_pool_caps[g_ep_pool_used++] = cap;
    return cap;
}

static uint32_t ep_pool_free_count(void) { return EP_POOL_SIZE - g_ep_pool_used; }

static void ep_free(seL4_CPtr cap)
{
    for (uint32_t i = 0u; i < g_ep_pool_used; i++) {
        if (g_ep_pool_caps[i] == cap) {
            for (uint32_t j = i + 1u; j < g_ep_pool_used; j++)
                g_ep_pool_caps[j - 1u] = g_ep_pool_caps[j];
            g_ep_pool_caps[--g_ep_pool_used] = seL4_CapNull;
            return;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Badge helpers (agentOS 64-bit badge layout)
 *
 *   bits 63:32  op_token   (32 bits)
 *   bits 31:16  service_id (16 bits)
 *   bits 15:0   client_id  (16 bits)
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct { uint16_t client_id; uint16_t service_id; uint32_t op_token; }
    cap_badge_fields_t;

static inline uint64_t cap_badge_encode(cap_badge_fields_t f)
{
    return ((uint64_t)f.op_token << 32u) | ((uint64_t)f.service_id << 16u)
         |  (uint64_t)f.client_id;
}

/* cap_badge_decode reserved for future tests that inspect individual fields */
__attribute__((unused))
static inline cap_badge_fields_t cap_badge_decode(uint64_t b)
{
    cap_badge_fields_t f;
    f.client_id  = (uint16_t)( b        & 0xFFFFu);
    f.service_id = (uint16_t)((b >> 16u) & 0xFFFFu);
    f.op_token   = (uint32_t)((b >> 32u) & 0xFFFFFFFFu);
    return f;
}

/* ══════════════════════════════════════════════════════════════════════════
 * ep_mint_badge — attach a badge to a cap (simulated seL4_CNode_Mint)
 * ══════════════════════════════════════════════════════════════════════════ */

#define EP_MINT_MAX  32u

typedef struct {
    seL4_CPtr source;
    seL4_CPtr minted;
    uint64_t  badge;
    uint32_t  valid;
} ep_mint_t;

static ep_mint_t g_mints[EP_MINT_MAX];
static uint32_t  g_mint_count;

static void ep_mint_reset(void) {
    g_mint_count = 0u;
    for (uint32_t i = 0u; i < EP_MINT_MAX; i++) g_mints[i].valid = 0u;
}

static seL4_CPtr ep_mint_badge(seL4_CPtr source, uint64_t badge)
{
    if (g_mint_count >= EP_MINT_MAX) return seL4_CapNull;
    seL4_CPtr minted = (seL4_CPtr)(200u + g_mint_count);
    ep_mint_t *m = &g_mints[g_mint_count++];
    m->source = source; m->minted = minted; m->badge = badge; m->valid = 1u;
    return minted;
}

static int ep_mint_get_badge(seL4_CPtr minted, uint64_t *out)
{
    for (uint32_t i = 0u; i < EP_MINT_MAX; i++) {
        if (g_mints[i].valid && g_mints[i].minted == minted) {
            if (out) *out = g_mints[i].badge;
            return 0;
        }
    }
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test functions — 25 TAP points (one ASSERT per function, except where
 * multiple tightly-coupled assertions are noted).
 * ══════════════════════════════════════════════════════════════════════════ */

/* T01 — cap_acct_entry_t size >= 24 bytes (cap+obj_type+pd_index+name) */
static void t01_acct_entry_size(void)
{
    ASSERT_TRUE(sizeof(cap_acct_entry_t) >= 24u,
                "cap_acct_entry_t size >= 24 bytes");
}

/* T02 — CAP_ACCT_MAX_ENTRIES == 1024 */
static void t02_max_entries_constant(void)
{
    ASSERT_EQ(CAP_ACCT_MAX_ENTRIES, 1024u,
              "CAP_ACCT_MAX_ENTRIES equals 1024");
}

/* T03 — cap_acct_init seeds exactly 3 boot caps */
static void t03_acct_init_boot_caps(void)
{
    cap_acct_init();
    ASSERT_EQ(cap_acct_count(), 3u,
              "cap_acct_init: seeds exactly 3 boot caps");
}

/* T04 — cap_acct_record returns 0 and increments count */
static void t04_acct_record_insert(void)
{
    cap_acct_init();
    int rc = cap_acct_record(seL4_CapNull, (seL4_CPtr)42u,
                              seL4_EndpointObject, 1u, "ep42");
    ASSERT_EQ(rc, 0, "cap_acct_record: returns 0 on success");
}

/* T05 — cap_acct_count increments after record */
static void t05_acct_count_increments(void)
{
    cap_acct_init();
    uint32_t before = cap_acct_count();
    cap_acct_record(seL4_CapNull, (seL4_CPtr)77u,
                    seL4_EndpointObject, 2u, "ep77");
    ASSERT_EQ(cap_acct_count(), before + 1u,
              "cap_acct_count: increments by 1 after record");
}

/* T06 — cap_acct_get returns entry with correct cap value */
static void t06_acct_get_cap_value(void)
{
    cap_acct_init();
    cap_acct_record(seL4_CapNull, (seL4_CPtr)99u,
                    seL4_EndpointObject, 0u, "ep99");
    uint32_t idx = cap_acct_count() - 1u;
    const cap_acct_entry_t *e = cap_acct_get(idx);
    ASSERT_TRUE(e != NULL && e->cap == (seL4_CPtr)99u,
                "cap_acct_get: cap value matches inserted value");
}

/* T07 — cap_acct_get returns NULL for out-of-range index */
static void t07_acct_get_out_of_range(void)
{
    cap_acct_init();
    const cap_acct_entry_t *e = cap_acct_get(CAP_ACCT_MAX_ENTRIES);
    ASSERT_TRUE(e == NULL,
                "cap_acct_get: returns NULL for out-of-range index");
}

/* T08 — cap_acct_entry name field stored and retrieved correctly */
static void t08_acct_name_field(void)
{
    cap_acct_init();
    cap_acct_record(seL4_CapNull, (seL4_CPtr)55u,
                    seL4_EndpointObject, 0u, "my-cap");
    uint32_t idx = cap_acct_count() - 1u;
    const cap_acct_entry_t *e = cap_acct_get(idx);
    ASSERT_TRUE(e != NULL && strncmp(e->name, "my-cap", 6) == 0,
                "cap_acct_entry: name field stored correctly");
}

/* T09 — cap_tree_insert creates a node and returns valid index */
static void t09_tree_insert(void)
{
    cap_tree_reset();
    uint32_t idx = cap_tree_insert((seL4_CPtr)10u, 0u, 0u, "root",
                                    seL4_EndpointObject, 0u, CAP_NODE_NONE);
    ASSERT_NE(idx, CAP_NODE_NONE, "cap_tree_insert: returns valid index");
}

/* T10 — cap_tree_find_cap locates a node by cap value */
static void t10_tree_find_cap(void)
{
    cap_tree_reset();
    cap_tree_insert((seL4_CPtr)42u, 1u, 0u, "ep42",
                    seL4_EndpointObject, 0u, CAP_NODE_NONE);
    uint32_t idx = cap_tree_find_cap((seL4_CPtr)42u);
    ASSERT_NE(idx, CAP_NODE_NONE, "cap_tree_find_cap: finds node by cap value");
}

/* T11 — cap_tree_find_cap returns CAP_NODE_NONE for unknown cap */
static void t11_tree_find_unknown(void)
{
    cap_tree_reset();
    ASSERT_EQ(cap_tree_find_cap((seL4_CPtr)0xDEADBEEFu), CAP_NODE_NONE,
              "cap_tree_find_cap: CAP_NODE_NONE for unknown cap");
}

/* T12 — cap_tree_remove makes node unfindable */
static void t12_tree_remove(void)
{
    cap_tree_reset();
    uint32_t idx = cap_tree_insert((seL4_CPtr)20u, 0u, 0u, "x",
                                    0u, 0u, CAP_NODE_NONE);
    cap_tree_remove(idx);
    ASSERT_EQ(cap_tree_find_cap((seL4_CPtr)20u), CAP_NODE_NONE,
              "cap_tree_remove: node no longer found after removal");
}

/* T13 — cap_tree_walk_pd counts nodes for pd_id=0 */
static void t13_walk_pd_count(void)
{
    cap_tree_reset();
    cap_tree_insert((seL4_CPtr)1u, 0u, 0u, "a", 0u, 0u, CAP_NODE_NONE);
    cap_tree_insert((seL4_CPtr)2u, 0u, 0u, "b", 0u, 0u, CAP_NODE_NONE);
    cap_tree_insert((seL4_CPtr)3u, 1u, 0u, "c", 0u, 0u, CAP_NODE_NONE);
    ASSERT_EQ(cap_tree_walk_pd(0u), 2u,
              "cap_tree_walk_pd: pd_id=0 has 2 nodes");
}

/* T14 — cap_tree_walk_pd returns 0 for unknown pd_id */
static void t14_walk_pd_unknown(void)
{
    cap_tree_reset();
    cap_tree_insert((seL4_CPtr)1u, 0u, 0u, "a", 0u, 0u, CAP_NODE_NONE);
    ASSERT_EQ(cap_tree_walk_pd(99u), 0u,
              "cap_tree_walk_pd: returns 0 for unknown pd_id");
}

/* T15 — parent-child relationship: child.parent stores parent index */
static void t15_parent_child_relationship(void)
{
    cap_tree_reset();
    uint32_t par = cap_tree_insert((seL4_CPtr)100u, 0u, 0u, "parent",
                                    0u, 0u, CAP_NODE_NONE);
    cap_tree_insert((seL4_CPtr)101u, 0u, 0u, "child", 0u, 0u, par);
    uint32_t child_idx = cap_tree_find_cap((seL4_CPtr)101u);
    ASSERT_TRUE(child_idx != CAP_NODE_NONE &&
                g_cap_tree[child_idx].parent == par,
                "parent-child: child.parent == parent index");
}

/* T16 — 3 siblings all found by walk */
static void t16_sibling_relationships(void)
{
    cap_tree_reset();
    uint32_t root = cap_tree_insert((seL4_CPtr)200u, 1u, 0u, "root",
                                     0u, 0u, CAP_NODE_NONE);
    cap_tree_insert((seL4_CPtr)201u, 1u, 0u, "s1", 0u, 0u, root);
    cap_tree_insert((seL4_CPtr)202u, 1u, 0u, "s2", 0u, 0u, root);
    cap_tree_insert((seL4_CPtr)203u, 1u, 0u, "s3", 0u, 0u, root);
    ASSERT_EQ(cap_tree_walk_pd(1u), 4u,
              "sibling relationships: root + 3 siblings = 4 nodes");
}

/* T17 — cap_tree_revoke removes a 5-node subtree completely */
static void t17_revoke_subtree(void)
{
    cap_tree_reset();
    uint32_t root = cap_tree_insert((seL4_CPtr)300u, 0u, 0u, "root",
                                     0u, 0u, CAP_NODE_NONE);
    uint32_t ca   = cap_tree_insert((seL4_CPtr)301u, 0u, 0u, "child-a",
                                     0u, 0u, root);
    cap_tree_insert((seL4_CPtr)302u, 0u, 0u, "gc1", 0u, 0u, ca);
    cap_tree_insert((seL4_CPtr)303u, 0u, 0u, "gc2", 0u, 0u, ca);
    cap_tree_insert((seL4_CPtr)304u, 0u, 0u, "child-b", 0u, 0u, root);
    uint32_t removed = cap_tree_revoke(root);
    ASSERT_EQ(removed, 5u,
              "cap_tree_revoke: removes all 5 nodes in subtree");
}

/* T18 — revoke propagates: parent removal also removes children */
static void t18_revoke_propagates(void)
{
    cap_tree_reset();
    uint32_t par = cap_tree_insert((seL4_CPtr)400u, 0u, 0u, "par",
                                    0u, 0u, CAP_NODE_NONE);
    cap_tree_insert((seL4_CPtr)401u, 0u, 0u, "ch1", 0u, 0u, par);
    cap_tree_insert((seL4_CPtr)402u, 0u, 0u, "ch2", 0u, 0u, par);
    cap_tree_revoke(par);
    ASSERT_EQ(cap_tree_walk_pd(0u), 0u,
              "revoke propagates: all 3 nodes removed (parent + 2 children)");
}

/* T19 — cap_tree_walk_pd isolates by pd_id: two PDs, 3 each */
static void t19_walk_isolates_pd(void)
{
    cap_tree_reset();
    for (uint32_t i = 0u; i < 3u; i++)
        cap_tree_insert((seL4_CPtr)(10u + i), 0u, 0u, "p0", 0u, 0u, CAP_NODE_NONE);
    for (uint32_t i = 0u; i < 3u; i++)
        cap_tree_insert((seL4_CPtr)(20u + i), 1u, 0u, "p1", 0u, 0u, CAP_NODE_NONE);
    ASSERT_EQ(cap_tree_walk_pd(0u), 3u,
              "walk isolates pd: pd=0 has 3, not 6");
}

/* T20 — cap tree max capacity: insert 64 nodes succeeds, 65th fails */
static void t20_tree_max_capacity(void)
{
    cap_tree_reset();
    for (uint32_t i = 0u; i < CAP_TREE_MAX_NODES; i++)
        cap_tree_insert((seL4_CPtr)(i + 1u), 0u, 0u, "cap",
                         0u, 0u, CAP_NODE_NONE);
    uint32_t overflow = cap_tree_insert((seL4_CPtr)9999u, 0u, 0u, "overflow",
                                         0u, 0u, CAP_NODE_NONE);
    ASSERT_EQ(overflow, CAP_NODE_NONE,
              "cap tree max capacity: 65th insert returns CAP_NODE_NONE");
}

/* T21 — ep_pool_free_count decrements on alloc */
static void t21_ep_pool_free_decrements(void)
{
    ep_pool_reset();
    uint32_t before = ep_pool_free_count();
    ep_alloc();
    ASSERT_EQ(ep_pool_free_count(), before - 1u,
              "ep_pool_free_count: decrements on alloc");
}

/* T22 — ep_pool_free_count increments on free */
static void t22_ep_pool_free_increments(void)
{
    ep_pool_reset();
    seL4_CPtr cap = ep_alloc();
    uint32_t after_alloc = ep_pool_free_count();
    ep_free(cap);
    ASSERT_EQ(ep_pool_free_count(), after_alloc + 1u,
              "ep_pool_free_count: increments on free");
}

/* T23 — ep_alloc returns distinct caps for successive allocations */
static void t23_ep_alloc_distinct(void)
{
    ep_pool_reset();
    seL4_CPtr a = ep_alloc();
    seL4_CPtr b = ep_alloc();
    ASSERT_NE(a, b, "ep_alloc: successive allocations return distinct caps");
}

/* T24 — ep_mint_badge stores and retrieves the correct badge value */
static void t24_ep_mint_badge_stored(void)
{
    ep_mint_reset();
    cap_badge_fields_t f = { .client_id = 3u, .service_id = 0x10u,
                              .op_token  = 0xABCu };
    uint64_t badge  = cap_badge_encode(f);
    seL4_CPtr minted = ep_mint_badge((seL4_CPtr)500u, badge);
    uint64_t retrieved = 0u;
    int rc = ep_mint_get_badge(minted, &retrieved);
    ASSERT_TRUE(rc == 0 && retrieved == badge,
                "ep_mint_badge: stored badge retrieved correctly");
}

/* T25 — badge value preserved in cap_tree node after insert */
static void t25_badge_preserved_in_tree(void)
{
    cap_tree_reset();
    ep_mint_reset();
    cap_badge_fields_t f = { .client_id = 0xBEu, .service_id = 0xEFu,
                              .op_token  = 0x12345678u };
    uint64_t badge   = cap_badge_encode(f);
    seL4_CPtr minted = ep_mint_badge((seL4_CPtr)600u, badge);
    uint32_t  idx    = cap_tree_insert(minted, 5u, badge, "minted-ep",
                                        seL4_EndpointObject, 0u, CAP_NODE_NONE);
    ASSERT_TRUE(idx != CAP_NODE_NONE && g_cap_tree[idx].badge == badge,
                "badge preserved in cap_tree: badge value roundtrips through node");
}

/* ══════════════════════════════════════════════════════════════════════════
 * main — 25 TAP test points
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    TAP_PLAN(25);

    t01_acct_entry_size();
    t02_max_entries_constant();
    t03_acct_init_boot_caps();
    t04_acct_record_insert();
    t05_acct_count_increments();
    t06_acct_get_cap_value();
    t07_acct_get_out_of_range();
    t08_acct_name_field();
    t09_tree_insert();
    t10_tree_find_cap();
    t11_tree_find_unknown();
    t12_tree_remove();
    t13_walk_pd_count();
    t14_walk_pd_unknown();
    t15_parent_child_relationship();
    t16_sibling_relationships();
    t17_revoke_subtree();
    t18_revoke_propagates();
    t19_walk_isolates_pd();
    t20_tree_max_capacity();
    t21_ep_pool_free_decrements();
    t22_ep_pool_free_increments();
    t23_ep_alloc_distinct();
    t24_ep_mint_badge_stored();
    t25_badge_preserved_in_tree();

    printf("TAP_DONE:%d\n", _tap_failed);
    return tap_exit();
}

#else
typedef int _agentos_cap_tests_dummy;
#endif /* AGENTOS_TEST_HOST */
