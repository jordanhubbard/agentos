/*
 * cap_lend.c — the capability-lending primitive: implementation.
 *
 * See cap_lend.h for the full design rationale. Summary of the two things
 * that must never be gotten wrong here:
 *
 *   - aos_cap_lend() mints into the LENDER'S OWN CNode (dest_cnode is the
 *     lender's self-reference cap, used as both Mint destination root and
 *     source root, since `original` lives in that same CSpace already).
 *   - aos_cap_lend_revoke() calls seL4_CNode_Revoke on the LENDER'S OWN
 *     `original` -- never the derivative -- because revoke removes the
 *     entire derivation subtree, and only the original sits at the root of
 *     that subtree.
 *
 * This module links against libs/pd-support/cap_lease.c for bookkeeping
 * (no seL4 calls there) and additionally remembers, per outstanding loan,
 * the (root, depth) pair needed to issue that revoke from a one-argument
 * call -- cap_lease.c itself knows nothing about seL4 types or CNode roots.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "cap_lend.h"

/* One record per outstanding loan, kept in lock-step with the lease table:
 * a record is filled exactly when the matching lease is opened, and
 * cleared exactly when that lease is closed (by aos_cap_lend_revoke).
 * Sized identically to the lease table so a free record slot is always
 * available whenever aos_lease_open() just succeeded. */
typedef struct {
    int        in_use;
    seL4_CPtr  original;
    seL4_CPtr  root;   /* CNode root used for the Mint; reused for Revoke */
    seL4_Word  depth;  /* radix, in bits, of that CNode */
    uint32_t   lease_id;
} cap_lend_record_t;

static aos_lease_table_t  g_lend_leases;
static cap_lend_record_t  g_lend_records[AOS_LEASE_MAX_ACTIVE];
static int                g_lend_initialized;

void aos_cap_lend_init(void)
{
    aos_lease_table_init(&g_lend_leases);
    for (uint32_t i = 0; i < AOS_LEASE_MAX_ACTIVE; i++) {
        g_lend_records[i] = (cap_lend_record_t){0};
    }
    g_lend_initialized = 1;
}

static void cap_lend_ensure_init(void)
{
    if (!g_lend_initialized) {
        aos_cap_lend_init();
    }
}

/*
 * Requested rights must be a STRICT subset of seL4_AllRights: at least one
 * of {grantreply, grant, read, write} must be dropped. seL4_CapRights_t has
 * no representation for "more than all rights", so the only way a mint can
 * fail to reduce authority is by requesting exactly seL4_AllRights -- that
 * is the case this rejects. Checked here, at the call site of
 * seL4_CNode_Mint, rather than trusted from the caller.
 */
static int cap_lend_rights_is_strict_subset(seL4_CapRights_t rights)
{
    seL4_Uint64 grant_reply = seL4_CapRights_get_capAllowGrantReply(rights);
    seL4_Uint64 grant       = seL4_CapRights_get_capAllowGrant(rights);
    seL4_Uint64 read        = seL4_CapRights_get_capAllowRead(rights);
    seL4_Uint64 write       = seL4_CapRights_get_capAllowWrite(rights);
    return !(grant_reply && grant && read && write);
}

static uint32_t cap_lend_pack_rights(seL4_CapRights_t rights)
{
    uint32_t bits = 0u;
    if (seL4_CapRights_get_capAllowGrantReply(rights)) bits |= 1u << 3;
    if (seL4_CapRights_get_capAllowGrant(rights))      bits |= 1u << 2;
    if (seL4_CapRights_get_capAllowRead(rights))       bits |= 1u << 1;
    if (seL4_CapRights_get_capAllowWrite(rights))      bits |= 1u << 0;
    return bits;
}

static cap_lend_record_t *cap_lend_find(seL4_CPtr original)
{
    for (uint32_t i = 0; i < AOS_LEASE_MAX_ACTIVE; i++) {
        if (g_lend_records[i].in_use && g_lend_records[i].original == original) {
            return &g_lend_records[i];
        }
    }
    return NULL;
}

static cap_lend_record_t *cap_lend_find_free(void)
{
    for (uint32_t i = 0; i < AOS_LEASE_MAX_ACTIVE; i++) {
        if (!g_lend_records[i].in_use) {
            return &g_lend_records[i];
        }
    }
    return NULL;
}

int aos_cap_lend(seL4_CPtr original, seL4_CPtr dest_cnode, seL4_Word dest_slot,
                  seL4_Word dest_depth, seL4_CapRights_t rights, seL4_Word badge)
{
    cap_lend_ensure_init();

    if (!cap_lend_rights_is_strict_subset(rights)) {
        return AOS_CAP_LEND_ERR_RIGHTS;
    }

    /* A capability already out on loan is tracked by its own record keyed
     * on `original`; refuse to double-lend the same original concurrently
     * so aos_cap_lend_revoke(original) always has exactly one loan to
     * tear down. */
    if (cap_lend_find(original) != NULL) {
        return AOS_CAP_LEND_ERR_LEASE;
    }

    cap_lend_record_t *rec = cap_lend_find_free();
    if (rec == NULL) {
        return AOS_CAP_LEND_ERR_LEASE;
    }

    uint32_t lease_id = 0u;
    if (aos_lease_open(&g_lend_leases, (uint32_t)badge,
                        cap_lend_pack_rights(rights), &lease_id) != 0) {
        return AOS_CAP_LEND_ERR_LEASE;
    }

    /*
     * Mint the derivative into the LENDER'S OWN CNode (dest_cnode is the
     * lender's self-reference capability, so it serves as both the Mint's
     * destination root and its source root -- `original` lives in this
     * same CSpace already). The resulting capability at (dest_cnode,
     * dest_slot) is a CHILD of `original` in the kernel's derivation tree:
     * badged, and carrying strictly fewer rights, exactly as asserted
     * above. It is not yet in the borrower's CSpace -- that happens by a
     * separate IPC capability transfer the caller performs after this
     * call returns success (see net_virt.c:567-570 for the
     * delete-receive-slot-then-SetCapReceivePath pattern the borrower
     * side must follow).
     */
    seL4_Error err = seL4_CNode_Mint(dest_cnode, dest_slot, (seL4_Uint8)dest_depth,
                                      dest_cnode, original, (seL4_Uint8)dest_depth,
                                      rights, badge);
    if (err != seL4_NoError) {
        (void)aos_lease_close(&g_lend_leases, lease_id);
        return AOS_CAP_LEND_ERR_MINT;
    }

    rec->in_use    = 1;
    rec->original  = original;
    rec->root      = dest_cnode;
    rec->depth     = dest_depth;
    rec->lease_id  = lease_id;
    return AOS_CAP_LEND_OK;
}

int aos_cap_lend_revoke(seL4_CPtr original)
{
    cap_lend_ensure_init();

    cap_lend_record_t *rec = cap_lend_find(original);
    if (rec == NULL) {
        return AOS_CAP_LEND_ERR_NOT_FOUND;
    }

    /*
     * Revoke the LENDER'S OWN ORIGINAL capability -- `original`, at
     * (rec->root, rec->depth), the exact (root, depth) pair this module
     * used to mint the derivative in aos_cap_lend(). seL4_CNode_Revoke
     * deletes every capability derived from the one it is given: the
     * derivative minted above, and anything the borrower further copied,
     * minted, or transferred from what it received over IPC. Revoking the
     * DERIVATIVE instead (i.e. the capability at dest_slot, or whatever
     * the borrower holds) would only remove descendants of THAT
     * capability and leave `original` -- the thing that still grants the
     * authority -- completely intact. That is the single easiest mistake
     * in this whole module, and the one that would let a loan outlive its
     * revocation while every test that only checks the direct borrower
     * still passes.
     */
    seL4_Error err = seL4_CNode_Revoke(rec->root, original, (seL4_Uint8)rec->depth);
    if (err != seL4_NoError) {
        return AOS_CAP_LEND_ERR_REVOKE;
    }

    (void)aos_lease_close(&g_lend_leases, rec->lease_id);
    *rec = (cap_lend_record_t){0};
    return AOS_CAP_LEND_OK;
}

const aos_lease_t *aos_cap_lend_lookup(seL4_CPtr original)
{
    cap_lend_ensure_init();

    cap_lend_record_t *rec = cap_lend_find(original);
    if (rec == NULL) {
        return NULL;
    }
    return aos_lease_get(&g_lend_leases, rec->lease_id);
}
