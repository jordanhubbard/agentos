/*
 * Trust anchor tier contract
 *
 * agentOS verifies every protection-domain image before spawning it. This
 * header defines the tiers that verification can run under and the policy
 * of which tiers gate boot. It is the tier model and the gating policy
 * only: reading keys, enrolling a MOK, and verifying signatures are a
 * later layer. There is no key material, no cryptography, and no seL4
 * dependency here -- this file and its implementation are host-testable.
 *
 * The tiers follow Linux's shim/MOK model:
 *
 *   AOS_ANCHOR_NONE      No key. Verification still runs (digests are
 *                        still computed and mismatches still reported
 *                        elsewhere in the boot path) but nothing gates
 *                        boot. Development only; establishes nothing
 *                        about image integrity.
 *
 *   AOS_ANCHOR_VENDOR    A public key fixed at build time (the vendor
 *                        root). Requires the vendor key to be present.
 *                        Gates boot: a mismatch refuses to proceed.
 *
 *   AOS_ANCHOR_MOK       A machine-owner key. Requires mok.present --
 *                        full stop; a MOK tier with no machine-owner
 *                        key enrolled is not a MOK machine, it is a
 *                        vendor machine wearing the wrong label, and
 *                        is rejected by aos_anchor_validate(). The
 *                        vendor key is OPTIONAL for this tier -- a
 *                        machine owner may run entirely on their own
 *                        key, with no vendor key at all. Gates boot.
 *
 *                        Three sub-states fall out of this:
 *                          - vendor only   -> vendor-signed images verify
 *                          - mok only      -> owner-signed images verify,
 *                                             and VENDOR-SIGNED IMAGES DO
 *                                             NOT -- agentOS's own release
 *                                             images are refused on a
 *                                             MOK-only machine unless the
 *                                             owner signs or counter-signs
 *                                             them. That is the intended
 *                                             meaning of "stand alone",
 *                                             not a defect.
 *                          - both present  -> either verifies
 *
 *                        Does not defend against the machine owner, who
 *                        can sign any image they choose -- under MOK-only
 *                        the owner is the SOLE root of trust, so this
 *                        sharpens rather than weakens that point. MOK
 *                        constrains remote compromise and third-party
 *                        tampering, never the local operator.
 *
 *   AOS_ANCHOR_HARDWARE  A stub arm for a future OTP-fused key or
 *                        firmware TPM key source. It validates
 *                        structurally but reports NOT AVAILABLE, and
 *                        does not gate boot. No claim is made about
 *                        TPM or measured-boot support existing today;
 *                        this arm exists only so that later hardware
 *                        key-source work is a new key source rather
 *                        than a redesign of this contract.
 *
 *                        READ THIS BEFORE MAKING THIS TIER REACHABLE.
 *                        AOS_ANCHOR_HARDWARE is the SECOND non-gating
 *                        tier -- aos_anchor_gates_boot() returns false
 *                        for it exactly as it does for AOS_ANCHOR_NONE.
 *                        Today that is harmless only because no build
 *                        can emit it: xtask's AnchorTier (xtask/src/
 *                        boot_manifest.rs) has no Hardware variant, so
 *                        select_anchor() cannot produce tier 3 and the
 *                        tier is safe by UNREACHABILITY, not by a check.
 *                        AOS_ANCHOR_NONE, the other non-gating tier, is
 *                        guarded by an explicit opt-in that must be
 *                        named literally (AGENTOS_TRUST_ANCHOR=none) and
 *                        a hard build error otherwise. This tier has
 *                        none of that discipline, because it has never
 *                        needed it.
 *
 *                        So: whoever adds a Hardware variant to wire up
 *                        an OTP or firmware-TPM key source inherits a
 *                        non-gating tier reachable from a build, with no
 *                        opt-in guarding it. Either give it the same
 *                        opt-in discipline AOS_ANCHOR_NONE has, or make
 *                        it gate (which is presumably the point of
 *                        having hardware-rooted keys at all) -- but do
 *                        not make it reachable and leave it non-gating.
 *                        That combination is the silent-downgrade shape
 *                        this whole contract exists to prevent.
 *
 * THE SINGLE MOST IMPORTANT RULE IN THIS CONTRACT: aos_anchor_gates_boot()
 * returns false ONLY for AOS_ANCHOR_NONE and AOS_ANCHOR_HARDWARE. It never
 * returns false as a fallback because a key happens to be missing. A
 * gating tier (VENDOR or MOK) with its required key(s) absent is an
 * incoherent state that aos_anchor_validate() must reject outright --
 * silently degrading such a state into a non-gating boot is exactly the
 * failure this design exists to prevent, because it would hand an
 * operator a system they believe is verified when it is not.
 */

#pragma once
#include <stdint.h>

#define AOS_ANCHOR_VERSION 1u

/* Result codes for aos_anchor_validate(). */
#define AOS_ANCHOR_OK            0
#define AOS_ANCHOR_ERR_VERSION  -1 /* version field does not match AOS_ANCHOR_VERSION */
#define AOS_ANCHOR_ERR_NULL     -2 /* state pointer was NULL */
#define AOS_ANCHOR_ERR_TIER     -3 /* tier value is not one of the defined enum values */
#define AOS_ANCHOR_ERR_KEY      -4 /* a gating tier is missing a key it requires */

typedef enum {
    AOS_ANCHOR_NONE     = 0,
    AOS_ANCHOR_VENDOR   = 1,
    AOS_ANCHOR_MOK      = 2,
    AOS_ANCHOR_HARDWARE = 3,
} aos_anchor_tier_t;

/*
 * AOS_ANCHOR_UNVERIFIED — deliberately NOT a member of aos_anchor_tier_t.
 *
 * It is the answer to "which tier is this box running under?" on an
 * architecture that runs no tier at all: RISC-V embeds no PD bundle, so
 * PD images there are not verified on any tier (docs/TCB.md). Reporting
 * any real tier on such a box would be a lie in the one place an operator
 * looks to find out what their machine enforces -- AOS_ANCHOR_NONE in
 * particular would read as "development anchor", which claims a tier was
 * chosen and that digests are being computed and reported. Neither is
 * true there.
 *
 * It lives here, rather than as a private constant in the inspect ABI,
 * for one reason: aos_anchor_tier_name() must stay the SINGLE source of
 * tier display strings (platform/inspect/inspect_snapshot.c calls it
 * rather than carrying its own table). A sentinel whose name lived
 * somewhere else would reintroduce exactly the split that was already
 * found and removed once.
 *
 * It gets the right behaviour from the existing policy functions with no
 * change to either, which is why it is a sentinel and not a fifth tier.
 * Be precise about WHY, because the obvious reading is wrong: both policy
 * switches are over uint32_t with a default: arm, so merely promoting this
 * to an enum variant would change nothing by itself. What the behaviour
 * actually rests on is that NEITHER SWITCH HAS A case LABEL FOR IT, so
 * both fall to their fail-closed default:. Adding one is the hazard --
 * see "Do NOT special-case this value" below. The properties are:
 *   - aos_anchor_validate() REJECTS it (AOS_ANCHOR_ERR_TIER), because no
 *     build may ever select it as its anchor -- it is a reporting value,
 *     not a selectable anchor;
 *   - aos_anchor_gates_boot() returns nonzero (gating) for it, via the
 *     fail-closed default for unknown tiers. It is never consulted on the
 *     bundle-less path, and if it ever were, refusing is the safe answer.
 * Do NOT "fix" either of those by special-casing this value.
 *
 * The value is far outside the tier range on purpose: a future tier can
 * be added at 4 without colliding, and an all-ones field is unmistakably
 * not a tier index.
 */
#define AOS_ANCHOR_UNVERIFIED 0xFFFFFFFFu

/* A single key slot from a key source. `present` is a boolean (0/1); the
 * key bytes are not read or interpreted at all at this layer. */
typedef struct {
    uint8_t  pubkey[32];
    uint32_t present;
} aos_anchor_key_t;

typedef struct {
    uint32_t         version; /* must equal AOS_ANCHOR_VERSION */
    uint32_t         tier;    /* one of aos_anchor_tier_t */
    aos_anchor_key_t vendor;
    aos_anchor_key_t mok;
} aos_anchor_state_t;

/*
 * aos_anchor_validate — structural and policy validation of an anchor
 * state. Does NOT read key bytes or perform any cryptography; it only
 * checks that the recorded state is internally coherent:
 *
 *   - version must match AOS_ANCHOR_VERSION, checked before anything
 *     else is examined;
 *   - the pointer must not be NULL;
 *   - tier must be one of the defined aos_anchor_tier_t values;
 *   - AOS_ANCHOR_VENDOR requires vendor.present;
 *   - AOS_ANCHOR_MOK requires mok.present (full stop); vendor.present
 *     is optional for this tier -- a machine owner may stand alone on
 *     their own key with no vendor key enrolled at all -- but the MOK
 *     itself is never optional, so MOK with vendor.present and
 *     mok.present absent is still rejected;
 *   - AOS_ANCHOR_NONE and AOS_ANCHOR_HARDWARE require no keys and
 *     validate regardless of what `present` says.
 *
 * Returns AOS_ANCHOR_OK on success, or one of the AOS_ANCHOR_ERR_*
 * codes above.
 */
int aos_anchor_validate(const aos_anchor_state_t *state);

/*
 * aos_anchor_gates_boot — whether this tier's verification result
 * should stop boot on failure. Returns nonzero (true) for AOS_ANCHOR_VENDOR
 * and AOS_ANCHOR_MOK, zero (false) ONLY for AOS_ANCHOR_NONE and
 * AOS_ANCHOR_HARDWARE. Never returns false as a fallback for a missing
 * key -- callers must run aos_anchor_validate() first and treat a
 * validation failure as fatal; this function assumes a state that has
 * already validated and only answers the gating policy question.
 *
 * NULL, an unknown tier, or an un-validated incoherent state are all
 * treated as gating (nonzero) -- fail closed rather than silently
 * permit an unverified boot.
 */
int aos_anchor_gates_boot(const aos_anchor_state_t *state);

/*
 * aos_anchor_tier_name — a short, distinct, non-empty, NUL-terminated
 * name for a tier, suitable for boot output an operator reads to tell a
 * development box from a production one. Returns a safe, non-NULL
 * string for an unknown tier value too (never NULL, never crashes).
 */
const char *aos_anchor_tier_name(uint32_t tier);
