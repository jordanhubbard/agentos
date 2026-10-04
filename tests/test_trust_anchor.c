/* Host test: trust anchor tier contract and gating policy. No seL4, no
 * key material, no cryptography -- this pins the tier model and the
 * policy of which tiers gate boot. See
 * kernel/agentos-root-task/include/contracts/trust_anchor.h for the
 * semantics being asserted.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "contracts/trust_anchor.h"

static void make_key(aos_anchor_key_t *k, int present)
{
    memset(k, 0, sizeof(*k));
    if (present) {
        memset(k->pubkey, 0x42, sizeof(k->pubkey));
    }
    k->present = present ? 1u : 0u;
}

static aos_anchor_state_t base_state(uint32_t tier)
{
    aos_anchor_state_t s;
    memset(&s, 0, sizeof(s));
    s.version = AOS_ANCHOR_VERSION;
    s.tier = tier;
    make_key(&s.vendor, 0);
    make_key(&s.mok, 0);
    return s;
}

int main(void)
{
    aos_anchor_state_t s;

    /* AOS_ANCHOR_VENDOR with a present vendor key validates and gates. */
    s = base_state(AOS_ANCHOR_VENDOR);
    make_key(&s.vendor, 1);
    assert(aos_anchor_validate(&s) == AOS_ANCHOR_OK);
    assert(aos_anchor_gates_boot(&s) != 0);

    /* AOS_ANCHOR_VENDOR with the vendor key absent is rejected -- a
     * gating tier with no key is incoherent and must fail loudly, never
     * silently degrade to non-gating. */
    s = base_state(AOS_ANCHOR_VENDOR);
    assert(aos_anchor_validate(&s) != AOS_ANCHOR_OK);
    /* This state is rejected by aos_anchor_validate() above and a real
     * caller should never reach gates_boot() with it. But if it is
     * asked anyway, it must still answer "gate" -- gates_boot() must
     * never read key presence and decide "no key, so don't gate",
     * which is the exact silent-degradation anti-pattern this whole
     * design exists to prevent. Do not delete this as testing an
     * impossible case: it is a regression guard on that one rule. */
    assert(aos_anchor_gates_boot(&s) != 0);

    /* AOS_ANCHOR_MOK with both vendor and mok present validates and
     * gates -- either key verifies an image on this machine. */
    s = base_state(AOS_ANCHOR_MOK);
    make_key(&s.vendor, 1);
    make_key(&s.mok, 1);
    assert(aos_anchor_validate(&s) == AOS_ANCHOR_OK);
    assert(aos_anchor_gates_boot(&s) != 0);

    /* RULING (supersedes the original "additive-only" design): the
     * vendor key is OPTIONAL for AOS_ANCHOR_MOK. A machine owner may
     * stand alone on their own key, with no vendor key enrolled at
     * all -- MOK with mok present and vendor ABSENT now validates and
     * gates. On such a machine, agentOS's own vendor-signed release
     * images are refused unless the owner signs or counter-signs them;
     * that is the intended meaning of "stand alone", not a defect. */
    s = base_state(AOS_ANCHOR_MOK);
    make_key(&s.mok, 1);
    assert(aos_anchor_validate(&s) == AOS_ANCHOR_OK);
    assert(aos_anchor_gates_boot(&s) != 0);

    /* AOS_ANCHOR_MOK requires the MOK itself -- full stop. Vendor
     * present but mok ABSENT is rejected: a MOK tier with no
     * machine-owner key enrolled is not a MOK machine, it is a vendor
     * machine wearing the wrong label, and the tier must be a
     * truthful recorded property of the artifact. */
    s = base_state(AOS_ANCHOR_MOK);
    make_key(&s.vendor, 1);
    assert(aos_anchor_validate(&s) != AOS_ANCHOR_OK);
    /* Same guard as the VENDOR-with-no-key case above: validate()
     * rejects this state, but if gates_boot() is asked about it
     * anyway it must not answer "don't gate". Not testing a reachable
     * case -- testing that gates_boot() never branches on key
     * presence at all. */
    assert(aos_anchor_gates_boot(&s) != 0);

    /* AOS_ANCHOR_MOK with NEITHER key present is rejected too -- it is
     * subsumed by the mok.present check above (mok absent either way),
     * but kept as its own case because it is the "gating tier with no
     * key at all" incoherent state the rest of this contract guards
     * against everywhere else. */
    s = base_state(AOS_ANCHOR_MOK);
    assert(aos_anchor_validate(&s) != AOS_ANCHOR_OK);
    assert(aos_anchor_gates_boot(&s) != 0);

    /* AOS_ANCHOR_NONE validates with no keys present and does not gate. */
    s = base_state(AOS_ANCHOR_NONE);
    assert(aos_anchor_validate(&s) == AOS_ANCHOR_OK);
    assert(aos_anchor_gates_boot(&s) == 0);

    /* AOS_ANCHOR_HARDWARE validates structurally (no keys needed, it's
     * a stub key source) but reports NOT AVAILABLE by not gating. */
    s = base_state(AOS_ANCHOR_HARDWARE);
    assert(aos_anchor_validate(&s) == AOS_ANCHOR_OK);
    assert(aos_anchor_gates_boot(&s) == 0);

    /* An unknown tier value is rejected. */
    s = base_state(99u);
    assert(aos_anchor_validate(&s) != AOS_ANCHOR_OK);

    /* A version mismatch is rejected before anything else is examined,
     * even if the rest of the state (an unknown tier) would also be
     * invalid on its own. */
    s = base_state(99u);
    s.version = AOS_ANCHOR_VERSION + 1u;
    assert(aos_anchor_validate(&s) == AOS_ANCHOR_ERR_VERSION);

    /* NULL is rejected rather than dereferenced. */
    assert(aos_anchor_validate(NULL) != AOS_ANCHOR_OK);
    /* gates_boot must also fail closed (treat as gating) on NULL rather
     * than crash or silently permit. */
    assert(aos_anchor_gates_boot(NULL) != 0);

    /* aos_anchor_tier_name returns a distinct, non-empty string for
     * every defined tier. */
    const char *n_none = aos_anchor_tier_name(AOS_ANCHOR_NONE);
    const char *n_vendor = aos_anchor_tier_name(AOS_ANCHOR_VENDOR);
    const char *n_mok = aos_anchor_tier_name(AOS_ANCHOR_MOK);
    const char *n_hw = aos_anchor_tier_name(AOS_ANCHOR_HARDWARE);
    const char *n_unknown = aos_anchor_tier_name(99u);

    assert(n_none != NULL && n_none[0] != '\0');
    assert(n_vendor != NULL && n_vendor[0] != '\0');
    assert(n_mok != NULL && n_mok[0] != '\0');
    assert(n_hw != NULL && n_hw[0] != '\0');
    assert(n_unknown != NULL && n_unknown[0] != '\0');

    assert(strcmp(n_none, n_vendor) != 0);
    assert(strcmp(n_none, n_mok) != 0);
    assert(strcmp(n_none, n_hw) != 0);
    assert(strcmp(n_vendor, n_mok) != 0);
    assert(strcmp(n_vendor, n_hw) != 0);
    assert(strcmp(n_mok, n_hw) != 0);
    assert(strcmp(n_unknown, n_none) != 0);
    assert(strcmp(n_unknown, n_vendor) != 0);
    assert(strcmp(n_unknown, n_mok) != 0);
    assert(strcmp(n_unknown, n_hw) != 0);

    /* AOS_ANCHOR_UNVERIFIED — the sentinel reported on architectures that
     * embed no PD bundle and therefore run no tier at all. Its three
     * properties are load-bearing and each is asserted here, because each
     * one falls out of it NOT being a member of aos_anchor_tier_t and a
     * future "tidy-up" that promoted it to a fifth enum variant would
     * silently change all three. See its comment in trust_anchor.h.
     *
     * 1. It has its own distinct, non-empty name, so an operator reading a
     *    boot banner or `agentctl inspect` on such a box is told the truth
     *    ("no PD bundle here") rather than being shown a tier. In
     *    particular it must NOT read as AOS_ANCHOR_NONE, which would claim
     *    a development anchor was chosen and that digests are computed. */
    const char *n_unver = aos_anchor_tier_name(AOS_ANCHOR_UNVERIFIED);
    assert(n_unver != NULL && n_unver[0] != '\0');
    assert(strcmp(n_unver, n_none) != 0);
    assert(strcmp(n_unver, n_vendor) != 0);
    assert(strcmp(n_unver, n_mok) != 0);
    assert(strcmp(n_unver, n_hw) != 0);
    assert(strcmp(n_unver, n_unknown) != 0);

    /* 2. No build may SELECT it as its anchor: aos_anchor_validate()
     *    rejects it exactly as it rejects any other non-tier value. It is
     *    a reporting value, not a selectable anchor. */
    s = base_state(AOS_ANCHOR_UNVERIFIED);
    assert(aos_anchor_validate(&s) != AOS_ANCHOR_OK);

    /* 3. It is NOT a third non-gating tier. aos_anchor_gates_boot() must
     *    report gating for it via the fail-closed default. Nothing on the
     *    bundle-less path consults this, but if anything ever does, the
     *    safe answer is "refuse", never "permit". */
    assert(aos_anchor_gates_boot(&s) != 0);

    printf("PASS: test_trust_anchor\n");
    return 0;
}
