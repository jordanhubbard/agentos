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
 * at (dest_cnode, dest_slot) -- NOT into the borrower's CSpace. `src_root`
 * names the CNode that `original` itself lives in (resolved, like every
 * seL4_CNode_Mint root argument, in the CALLER'S OWN CSpace) and `dest_cnode`
 * names the CNode the derivative is minted into. seL4_CNode_Mint treats
 * these as entirely independent roots -- nothing requires them to be equal
 * capabilities, even though in the common case (a lender minting from, and
 * into, its own CNode) they name the SAME underlying CNode object via the
 * same self-referencing capability (the pattern used throughout this tree:
 * e.g. AOS_QUEUE_SERVICE_CNODE, AOS_GUEST_RAM_SELF_CNODE -- a capability to
 * a PD's own CNode, copied into that CNode by the root task at boot so the
 * PD can name itself as the root of its own CNode operations). Callers MUST
 * pass their own self-reference as `src_root` whenever `original` lives in
 * their own CSpace (the only case this module is designed for); passing
 * anything else makes aos_cap_lend_revoke() resolve `original` in the wrong
 * CSpace later, since revoke replays exactly the `(src_root, src_depth)`
 * this call recorded.
 *
 * The derivative minted here is "ready for transfer": a separate IPC send
 * (seL4_SetCap + seL4_Send/Call, with the borrower having already prepared
 * its receive slot -- see net_virt.c:567-570 for the delete-before-receive
 * pattern this tree always follows) moves a further-derived copy into the
 * borrower's CSpace. This module does not perform that IPC itself, because
 * the message-loop shape differs per PD; it only produces the capability
 * that is ready to go out in one.
 *
 * Rights must strictly reduce. seL4 gives userspace no syscall to read back
 * a capability's current rights, and seL4_CNode_Mint itself masks the
 * requested rights against the source capability's rights (rights ∩
 * src_rights) -- so a mint can never WIDEN authority no matter what is
 * requested; widening is simply unrepresentable at this interface. The one
 * thing a caller COULD do wrong is request no reduction at all, i.e. ask
 * for exactly seL4_AllRights. aos_cap_lend refuses that one case --
 * requested `rights` must drop at least one of {read, write, grant,
 * grantreply} relative to seL4_AllRights -- before any seL4 invocation,
 * which is what the subsetting invariant actually requires: a loan must
 * hand over not-everything, even when the lender holds everything (the
 * normal case for an object it created or was granted outright at boot).
 * This is checked at the call site (inside aos_cap_lend, before the
 * seL4_CNode_Mint invocation) rather than trusted from the caller.
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
 * remembers, per outstanding loan, the exact `(src_root, src_depth)` pair
 * the caller passed to aos_cap_lend() -- the CNode `original` itself lives
 * in -- keyed by `original`. That association -- and the associated
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
/* seL4_CNode_Revoke SUCCEEDED -- authority has been withdrawn, this is not
 * a revoke failure -- but closing the associated lease in the bookkeeping
 * table afterwards failed. The loan record is left in place (not cleared)
 * so the lease_id and loan metadata are not lost and the table slot is not
 * silently leaked; a caller may retry aos_cap_lend_revoke(original), which
 * will attempt the (now idempotent at the seL4 layer -- Revoke on an
 * already-empty subtree is a no-op) revoke again and retry the lease
 * close. */
#define AOS_CAP_LEND_ERR_LEASE_CLOSE (-6)

/*
 * Reset all internal state (the lease table and the original->loan-record
 * map) to empty. Safe to call at PD startup before any aos_cap_lend call;
 * idempotent. Primarily exists so tests can get a clean slate.
 */
void aos_cap_lend_init(void);

/*
 * Mint a badged, rights-reduced derivative of `original` into
 * (dest_cnode, dest_slot), ready to be handed to a borrower by IPC
 * capability transfer.
 *
 * `src_root` is the CNode `original` itself lives in, and `src_depth` its
 * radix in bits -- both resolved, like every seL4_CNode_Mint root
 * argument, in the CALLER'S OWN CSpace. `dest_cnode`/`dest_slot`/
 * `dest_depth` name where the derivative lands. In the normal case (a
 * lender minting from its own CNode back into its own CNode) `src_root`
 * and `dest_cnode` are the SAME self-referencing capability (see the file
 * header) and `src_depth` equals `dest_depth` -- but they are independent
 * parameters and the kernel does not require them to coincide. Pass your
 * own self-reference as `src_root` whenever `original` lives in your own
 * CSpace, which is the only case this module is designed for:
 * aos_cap_lend_revoke() later resolves `original` using exactly the
 * `(src_root, src_depth)` recorded here.
 *
 * Returns AOS_CAP_LEND_OK on success. On any failure, no capability is
 * minted and no lease is left open.
 */
int aos_cap_lend(seL4_CPtr src_root, seL4_CPtr original, seL4_Word src_depth,
                  seL4_CPtr dest_cnode, seL4_Word dest_slot, seL4_Word dest_depth,
                  seL4_CapRights_t rights, seL4_Word badge);

/*
 * Revoke the lender's own `original` capability -- not any derivative --
 * tearing down every descendant in the kernel's derivation tree: the
 * capability aos_cap_lend minted, and anything the borrower further
 * delegated from what it received. Closes the associated lease.
 *
 * Returns AOS_CAP_LEND_OK on success, AOS_CAP_LEND_ERR_NOT_FOUND if
 * `original` has no outstanding loan recorded, AOS_CAP_LEND_ERR_REVOKE if
 * the seL4_CNode_Revoke invocation itself failed (authority may or may not
 * have been withdrawn), or AOS_CAP_LEND_ERR_LEASE_CLOSE if seL4_CNode_Revoke
 * succeeded (authority WAS withdrawn) but closing the lease record failed.
 */
int aos_cap_lend_revoke(seL4_CPtr original);

/*
 * Look up the lease recorded for an outstanding loan of `original`.
 * Returns NULL if `original` was never lent, or its loan was already
 * revoked. For reporting only; this layer does not interpret lease state.
 */
const aos_lease_t *aos_cap_lend_lookup(seL4_CPtr original);
