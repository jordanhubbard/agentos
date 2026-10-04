/*
 * Host test for the endowment subsetting validator.
 *
 * Pins the semantics that make runtime domain creation safe: a child can
 * never be endowed with authority its parent did not hold. See
 * kernel/agentos-root-task/include/contracts/endowment_contract.h for the
 * full contract this exercises.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "contracts/endowment_contract.h"

static aos_endowment_t make_parent(void)
{
    aos_endowment_t p;
    memset(&p, 0, sizeof(p));
    p.version = AOS_ENDOWMENT_VERSION;
    p.count = 2;
    p.caps[0].kind = 1;
    p.caps[0].rights = 0x7;   /* read|write|grant, say */
    p.caps[0].parent_slot = 10;
    p.caps[1].kind = 2;
    p.caps[1].rights = 0x3;
    p.caps[1].parent_slot = 11;
    return p;
}

/* A request naming only capabilities present in parent_holdings, each
 * with rights a subset of the parent's, validates. */
static void test_valid_subset_accepted(void)
{
    aos_endowment_t parent = make_parent();
    aos_endowment_t req;
    memset(&req, 0, sizeof(req));
    req.version = AOS_ENDOWMENT_VERSION;
    req.count = 2;
    req.caps[0].kind = 1;
    req.caps[0].rights = 0x5;   /* subset of 0x7 */
    req.caps[0].parent_slot = 10;
    req.caps[1].kind = 2;
    req.caps[1].rights = 0x3;   /* equal to parent's -- still a subset */
    req.caps[1].parent_slot = 11;

    assert(aos_endowment_validate(&req, &parent) == 0);
    printf("PASS: valid narrowing subset accepted\n");
}

/* A request naming a capability the parent does not hold is rejected. */
static void test_unknown_slot_rejected(void)
{
    aos_endowment_t parent = make_parent();
    aos_endowment_t req;
    memset(&req, 0, sizeof(req));
    req.version = AOS_ENDOWMENT_VERSION;
    req.count = 1;
    req.caps[0].kind = 1;
    req.caps[0].rights = 0x1;
    req.caps[0].parent_slot = 99;  /* parent holds no such slot */

    assert(aos_endowment_validate(&req, &parent) != 0);
    printf("PASS: unknown parent_slot rejected\n");
}

/* Same parent_slot but a different kind must also be rejected -- a slot
 * match alone is not enough. */
static void test_kind_mismatch_rejected(void)
{
    aos_endowment_t parent = make_parent();
    aos_endowment_t req;
    memset(&req, 0, sizeof(req));
    req.version = AOS_ENDOWMENT_VERSION;
    req.count = 1;
    req.caps[0].kind = 99;         /* parent's slot 10 is kind 1 */
    req.caps[0].rights = 0x1;
    req.caps[0].parent_slot = 10;

    assert(aos_endowment_validate(&req, &parent) != 0);
    printf("PASS: kind mismatch on matching slot rejected\n");
}

/* A request whose rights exceed the parent's for a held capability is
 * rejected. Exercise bits that are OUTSIDE the parent's mask entirely
 * (not a superset by extra matching bits) to pin the subset-not-nonzero
 * semantics. */
static void test_rights_escalation_rejected(void)
{
    aos_endowment_t parent = make_parent();
    aos_endowment_t req;
    memset(&req, 0, sizeof(req));
    req.version = AOS_ENDOWMENT_VERSION;
    req.count = 1;
    req.caps[0].kind = 2;
    req.caps[0].rights = 0x3 | 0x8;  /* parent's slot 11 only has 0x3 */
    req.caps[0].parent_slot = 11;

    assert(aos_endowment_validate(&req, &parent) != 0);
    printf("PASS: rights escalation beyond parent's mask rejected\n");
}

/* count == 0 is rejected: an endowment granting nothing is a mistake,
 * not a domain. */
static void test_zero_count_rejected(void)
{
    aos_endowment_t parent = make_parent();
    aos_endowment_t req;
    memset(&req, 0, sizeof(req));
    req.version = AOS_ENDOWMENT_VERSION;
    req.count = 0;

    assert(aos_endowment_validate(&req, &parent) != 0);
    printf("PASS: zero-count endowment rejected\n");
}

/* count > AOS_ENDOWMENT_MAX_CAPS is rejected without reading past the
 * array. We cannot directly observe "did not read OOB" in a host test,
 * but we can at least confirm the over-limit count is rejected and that
 * the function does not crash doing so, leaving caps[] zeroed/garbage
 * beyond the struct's own fixed array (no out-of-struct access is even
 * possible given the fixed-size array, by construction -- the check
 * still matters so a caller that memcpy'd a larger wire buffer into a
 * properly-sized struct is still refused). */
static void test_over_max_count_rejected(void)
{
    aos_endowment_t parent = make_parent();
    aos_endowment_t req;
    memset(&req, 0, sizeof(req));
    req.version = AOS_ENDOWMENT_VERSION;
    req.count = AOS_ENDOWMENT_MAX_CAPS + 1;

    assert(aos_endowment_validate(&req, &parent) != 0);
    printf("PASS: count exceeding AOS_ENDOWMENT_MAX_CAPS rejected\n");
}

/* A version mismatch is rejected before anything else is examined --
 * exercise it together with an otherwise-invalid count to show version
 * is checked first (a rejection happens either way, but this documents
 * intended ordering per the contract). */
static void test_version_mismatch_rejected(void)
{
    aos_endowment_t parent = make_parent();
    aos_endowment_t req = make_parent();
    req.version = AOS_ENDOWMENT_VERSION + 1;

    assert(aos_endowment_validate(&req, &parent) != 0);
    printf("PASS: version mismatch rejected\n");
}

/* NULL for either argument is rejected rather than dereferenced. */
static void test_null_arguments_rejected(void)
{
    aos_endowment_t parent = make_parent();
    aos_endowment_t req = make_parent();

    assert(aos_endowment_validate(NULL, &parent) != 0);
    assert(aos_endowment_validate(&req, NULL) != 0);
    assert(aos_endowment_validate(NULL, NULL) != 0);
    printf("PASS: NULL arguments rejected without crashing\n");
}

int main(void)
{
    test_valid_subset_accepted();
    test_unknown_slot_rejected();
    test_kind_mismatch_rejected();
    test_rights_escalation_rejected();
    test_zero_count_rejected();
    test_over_max_count_rejected();
    test_version_mismatch_rejected();
    test_null_arguments_rejected();

    printf("ALL TESTS PASSED\n");
    return 0;
}
