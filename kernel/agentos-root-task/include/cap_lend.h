/*
 * cap_lend.h — the capability-lending primitive (caretaker pattern)
 *
 * This is the seL4 mechanics layer of T5 capability lending. cap_lease.c
 * (see contracts/lease_contract.h) is pure bookkeeping with no seL4 calls;
 * this module is the opposite: it is the thin layer that actually mints,
 * transfers, and revokes capabilities, and it records what it did in a
 * lease table so a lender can report and close its own loans.
 *
 * Architecture (see docs/superpowers/plans/2026-10-04-t5-capability-lending.md):
 * a library, not a service. The holder of a capability ("the lender") mints
 * a badged, rights-reduced derivative of its own capability and transfers it
 * to a borrower over an existing IPC endpoint. There is no broker: only the
 * domain that already holds the original can call aos_cap_lend on it, because
 * minting requires the original as the Mint's source capability. A component
 * that could mint on behalf of any caller would be ambient authority; this
 * module never does that.
 *
 * ── aos_cap_lend: mint the derivative ───────────────────────────────────────
 *
 * aos_cap_lend mints a badged copy of `original` into the lender's OWN CNode
 * at (dest_cnode, dest_slot) -- NOT into the borrower's CSpace. `dest_cnode`
 * is the lender's self-referencing CNode capability (the same pattern used
 * throughout this tree: e.g. AOS_QUEUE_SERVICE_CNODE, AOS_GUEST_RAM_SELF_CNODE
 * -- a capability to a PD's own CNode, copied into that CNode by the root
 * task at boot so the PD can name itself as the root of its own CNode
 * operations). Because `original` lives in that same CSpace, `dest_cnode`
 * serves as both the Mint's destination root and its source root.
 *
 * The derivative minted here is "ready for transfer": a separate IPC send
 * (seL4_SetCap + seL4_Send/Call, with the borrower having already prepared
 * its receive slot -- see net_virt.c:567-570 for the delete-before-receive
 * pattern this tree always follows) moves a further-derived copy into the
 * borrower's CSpace. This module does not perform that IPC itself, because
 * the message-loop shape differs per PD; it only produces the capability
 * that is ready to go out in one.
 *
 * Rights must strictly reduce. The lender is assumed to hold full rights on
 * any original it owns outright (the normal case for an object a PD created
 * or was granted at boot). aos_cap_lend asserts that the requested `rights`
 * drop at least one of {read, write, grant, grantreply} relative to
 * seL4_AllRights -- a mint that preserves full rights would hand the
 * borrower everything the lender has, breaking the subsetting invariant the
 * whole trust-lending track rests on. This is checked at the call site
 * (inside aos_cap_lend, before the seL4_CNode_Mint invocation) rather than
 * trusted from the caller.
 *
 * ── aos_cap_lend_revoke: revoke the ORIGINAL ────────────────────────────────
 *
 * aos_cap_lend_revoke(original) calls seL4_CNode_Revoke on the LENDER'S OWN
 * `original` capability -- never on the derivative minted above, and never
 * on whatever the borrower received. seL4_CNode_Revoke deletes every
 * descendant of the capability it is given: the derivative this module
 * minted, anything the borrower further copied or minted from its received
 * capability, all of it. Revoking the derivative instead would only tear
 * down *its* descendants and leave `original` (and the derivative's sibling
 * copies, if any) fully intact -- the single easiest mistake to make here,
 * and the one that makes a lease outlive its revocation.
 *
 * To make this possible from a one-argument revoke call, this module
 * remembers, per outstanding loan, the (root, depth) pair that was used to
 * mint it, keyed by `original`. That association -- and the associated
 * lease -- is recorded in an internal aos_lease_table_t so a lender can
 * also report what it currently has on loan and to whom (see
 * aos_cap_lend_lookup). Per the "ledger cannot verify a lease" note in the
 * design doc: this record is only as honest as this module's bookkeeping.
 * The kernel enforces the underlying authority relation regardless of
 * whether a lease was ever recorded here.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#include <sel4/sel4.h>

#include "contracts/lease_contract.h"

/* Return codes for aos_cap_lend / aos_cap_lend_revoke. */
#define AOS_CAP_LEND_OK            0
/* Requested rights are not a strict subset of seL4_AllRights: the mint
 * would preserve (or, were it representable, widen) the lender's full
 * authority instead of reducing it. Refused before any seL4 invocation. */
#define AOS_CAP_LEND_ERR_RIGHTS   (-1)
/* The internal lease table is at capacity (AOS_LEASE_MAX_ACTIVE loans
 * already open); no seL4 invocation was made. */
#define AOS_CAP_LEND_ERR_LEASE    (-2)
/* seL4_CNode_Mint failed; the lease opened for this attempt was closed
 * again and no capability was minted. */
#define AOS_CAP_LEND_ERR_MINT     (-3)
/* aos_cap_lend_revoke was called on a `original` this module has no
 * outstanding-loan record for (never lent, or already revoked). */
#define AOS_CAP_LEND_ERR_NOT_FOUND (-4)
/* seL4_CNode_Revoke failed. The lease record is left in place so a caller
 * can retry; authority may or may not have actually been withdrawn,
 * depending on where the kernel call failed. */
#define AOS_CAP_LEND_ERR_REVOKE   (-5)

/*
 * Reset all internal state (the lease table and the original->loan-record
 * map) to empty. Safe to call at PD startup before any aos_cap_lend call;
 * idempotent. Primarily exists so tests can get a clean slate.
 */
void aos_cap_lend_init(void);

/*
 * Mint a badged, rights-reduced derivative of `original` into the lender's
 * own CNode at (dest_cnode, dest_slot), ready to be handed to a borrower by
 * IPC capability transfer. `dest_cnode` must be the lender's own
 * self-referencing CNode capability (see the file header); `original` must
 * live in that same CSpace. `dest_depth` is the radix, in bits, of that
 * CNode (used as both the Mint's dest_depth and src_depth, since source and
 * destination are the same CSpace).
 *
 * Returns AOS_CAP_LEND_OK on success. On any failure, no capability is
 * minted and no lease is left open.
 */
int aos_cap_lend(seL4_CPtr original, seL4_CPtr dest_cnode, seL4_Word dest_slot,
                  seL4_Word dest_depth, seL4_CapRights_t rights, seL4_Word badge);

/*
 * Revoke the lender's own `original` capability -- not any derivative --
 * tearing down every descendant in the kernel's derivation tree: the
 * capability aos_cap_lend minted, and anything the borrower further
 * delegated from what it received. Closes the associated lease.
 *
 * Returns AOS_CAP_LEND_OK on success, AOS_CAP_LEND_ERR_NOT_FOUND if
 * `original` has no outstanding loan recorded, AOS_CAP_LEND_ERR_REVOKE if
 * the seL4_CNode_Revoke invocation itself failed.
 */
int aos_cap_lend_revoke(seL4_CPtr original);

/*
 * Look up the lease recorded for an outstanding loan of `original`.
 * Returns NULL if `original` was never lent, or its loan was already
 * revoked. For reporting only; this layer does not interpret lease state.
 */
const aos_lease_t *aos_cap_lend_lookup(seL4_CPtr original);
