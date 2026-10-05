/*
 * Endowment descriptor and subsetting-validation contract.
 *
 * A host-testable description of what a parent protection domain intends
 * to grant a child domain it creates at run time. This header defines the
 * descriptor only -- validation that a requested endowment never exceeds
 * what the parent actually holds is the entire safety argument for
 * runtime domain creation:
 *
 *     No domain ever holds authority its parent did not hold.
 *
 * seL4 enforces the mechanism for free: a parent cannot mint a capability
 * derivative it does not possess. What this layer adds is a check BEFORE
 * any seL4 object is touched, so a malformed or over-reaching request is
 * refused up front rather than relying solely on the kernel to reject the
 * mint at the point of no return.
 *
 * This layer makes NO seL4 calls, includes NO seL4 headers, and knows
 * nothing about actual capability types or CPtrs. `parent_slot` is an
 * opaque index into whatever table the caller uses to resolve a slot to a
 * real capability; resolving it is the spawn primitive's job (a later,
 * on-target task), not this one's.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#include <stdint.h>

#define AOS_ENDOWMENT_VERSION    1u

/* Wire/API cap: an endowment names at most this many capabilities. Chosen
 * to keep aos_endowment_t small and fixed-size (no dynamic allocation in
 * this layer) while covering any child PD's realistic initial grant. */
#define AOS_ENDOWMENT_MAX_CAPS   8u

/*
 * One capability within an endowment request or a parent's holdings
 * declaration.
 *
 *   kind        -- capability class (e.g. a frame, a queue cap, an
 *                   untyped pool slice). Caller-defined enumeration;
 *                   this layer only compares for equality.
 *   rights      -- bitmask of rights requested/held on that capability.
 *                   Compared as a SUBSET test, never equality and never
 *                   "non-zero": a request narrows or matches the
 *                   parent's rights, it never need use all of them.
 *   parent_slot -- opaque identifier of which of the parent's held
 *                   capabilities this entry names. Matched by equality
 *                   against entries in parent_holdings.
 */
typedef struct {
    uint32_t kind;
    uint32_t rights;
    uint32_t parent_slot;
} aos_endow_cap_t;

/*
 * A full endowment descriptor: either a request ("I want to grant my
 * child these capabilities") or a declaration of what a parent actually
 * holds, depending on context. Same shape for both sides of the
 * subsetting check.
 */
typedef struct {
    uint32_t         version;
    uint32_t         count;
    aos_endow_cap_t  caps[AOS_ENDOWMENT_MAX_CAPS];
} aos_endowment_t;

/*
 * Validate that `req` is a safe endowment to grant, given that the
 * granting parent actually holds `parent_holdings`.
 *
 * Returns 0 if `req` validates, nonzero (rejected) otherwise. Rejection
 * reasons, in the order checked:
 *
 *   - either pointer is NULL
 *   - req->version != AOS_ENDOWMENT_VERSION
 *   - req->count == 0 (an endowment granting nothing is a mistake, not
 *     a domain)
 *   - req->count > AOS_ENDOWMENT_MAX_CAPS (checked before any array
 *     element is read -- req is attacker-shaped data in the general
 *     case)
 *   - any req->caps[i] whose parent_slot/kind does not match an entry
 *     in parent_holdings
 *   - any req->caps[i] whose rights are not a subset of the matching
 *     parent_holdings entry's rights -- tested as
 *     (req_rights & ~parent_rights) == 0, NOT equality and NOT
 *     "req_rights != 0"
 *
 * Does not inspect parent_holdings->version: the parent's own holdings
 * record is this domain's internal state, not attacker-shaped input.
 */
int aos_endowment_validate(const aos_endowment_t *req,
                            const aos_endowment_t *parent_holdings);
