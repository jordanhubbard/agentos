/*
 * Trust anchor tier contract -- implementation
 *
 * See kernel/agentos-root-task/include/contracts/trust_anchor.h for the
 * full semantics. This file implements only the tier model and the
 * gating policy: no key material is read or interpreted, no
 * cryptography is performed, and there is no seL4 dependency here.
 */

#include <stddef.h>

#include "contracts/trust_anchor.h"

int aos_anchor_validate(const aos_anchor_state_t *state)
{
    if (state == NULL) {
        return AOS_ANCHOR_ERR_NULL;
    }

    /* Version mismatch is rejected before anything else is examined. */
    if (state->version != AOS_ANCHOR_VERSION) {
        return AOS_ANCHOR_ERR_VERSION;
    }

    switch (state->tier) {
    case AOS_ANCHOR_NONE:
        /* Development tier: no key required, none consulted. Keys being
         * present or absent does not matter here -- there is nothing to
         * gate on in this tier. */
        return AOS_ANCHOR_OK;

    case AOS_ANCHOR_VENDOR:
        /* Gating tier: the vendor key MUST be present. A gating tier
         * with no key is incoherent and must fail loudly rather than
         * silently become non-gating. */
        if (!state->vendor.present) {
            return AOS_ANCHOR_ERR_KEY;
        }
        return AOS_ANCHOR_OK;

    case AOS_ANCHOR_MOK:
        /* Gating tier: BOTH the vendor key and the MOK must be present.
         * MOK is additive to the vendor root, never a replacement for
         * it -- a MOK tier with no vendor key would let an
         * owner-enrolled key displace the vendor root entirely, which
         * this contract must refuse. */
        if (!state->vendor.present || !state->mok.present) {
            return AOS_ANCHOR_ERR_KEY;
        }
        return AOS_ANCHOR_OK;

    case AOS_ANCHOR_HARDWARE:
        /* Stub key source: validates structurally. It reports NOT
         * AVAILABLE via aos_anchor_gates_boot() returning false, not via
         * validation failure -- the shape of the tier is real even
         * though the key source behind it is not implemented yet. */
        return AOS_ANCHOR_OK;

    default:
        return AOS_ANCHOR_ERR_TIER;
    }
}

int aos_anchor_gates_boot(const aos_anchor_state_t *state)
{
    /* Fail closed: NULL, an unvalidated state, or a tier this function
     * does not recognize are all treated as gating. This function must
     * never be the place that silently turns a missing key into a
     * non-gating boot -- that failure mode is rejected by
     * aos_anchor_validate() instead, and callers must call it first. */
    if (state == NULL) {
        return 1;
    }

    switch (state->tier) {
    case AOS_ANCHOR_NONE:
    case AOS_ANCHOR_HARDWARE:
        return 0;

    case AOS_ANCHOR_VENDOR:
    case AOS_ANCHOR_MOK:
        return 1;

    default:
        /* Unknown tier: fail closed rather than permit an unverified
         * boot under a tier this code does not understand. */
        return 1;
    }
}

const char *aos_anchor_tier_name(uint32_t tier)
{
    switch (tier) {
    case AOS_ANCHOR_NONE:
        return "none (development, not gating)";
    case AOS_ANCHOR_VENDOR:
        return "vendor";
    case AOS_ANCHOR_MOK:
        return "machine-owner";
    case AOS_ANCHOR_HARDWARE:
        return "hardware (not available)";
    default:
        return "unknown";
    }
}
