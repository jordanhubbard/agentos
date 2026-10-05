# `dev_signing_key.seed`

This is a raw 32-byte Ed25519 seed, generated with `openssl rand -out
dev_signing_key.seed 32` and committed to this repository on purpose.

**It is deliberately not a secret.** The Makefile (`kernel/agentos-root-task/
Makefile`) defaults `AGENTOS_BUNDLE_SIGNING_KEY` to this file's path so that a
developer building agentOS from a fresh checkout, with no key material of
their own, still gets a boot manifest (see `kernel/agentos-root-task/include/
boot_manifest.h` and `xtask/src/boot_manifest.rs`) signed and verified under
the `AOS_ANCHOR_VENDOR` trust anchor tier — rather than the build either
failing outright or silently producing a non-gating image. The matching
public key is derived from this seed and compiled into the root task at
build time; the generated boot manifest header also carries
`AOS_BOOT_MANIFEST_DEV_SIGNED 1` whenever this key was used, and the root
task prints an unmissable `[rt] WARNING: DEVELOPMENT-signed image` line at
boot when it is.

**It must never be used to sign anything that will be presented as, or
mistaken for, a production image.** Anyone with a copy of this repository
can sign a manifest this key's matching root task will accept; it
establishes nothing about provenance or integrity beyond "built from source
that still had the default key." Note that the dev-signed indicator is
**distinct** from the trust anchor tier: a build can be tier `AOS_ANCHOR_
VENDOR` (gates boot) *and* dev-signed at the same time (the Makefile default
case above) — the tier says whether a mismatch stops boot, the dev-signed
flag says which key actually signed it. Collapsing those two into one
boolean was T3's original design; T10 deliberately keeps them separate.

## Trust anchor tiers (T10)

agentOS verifies every protection-domain image before spawning it, against
one of three trust anchor tiers selected at build time. See
`kernel/agentos-root-task/include/contracts/trust_anchor.h` and
`libs/pd-support/trust_anchor.c` for the tier contract and gating policy,
and `docs/TCB.md` ("Protection-domain image verification") for what each
tier establishes as a platform claim.

| Tier | Selected by | Gates boot? |
| --- | --- | --- |
| `AOS_ANCHOR_VENDOR` | `AGENTOS_BUNDLE_SIGNING_KEY` set (alone) | yes |
| `AOS_ANCHOR_MOK` | `AGENTOS_MOK_SIGNING_KEY` set (with or without the vendor var) | yes |
| `AOS_ANCHOR_NONE` | neither key set, **and** `AGENTOS_TRUST_ANCHOR=none` given explicitly | **no** |

Selection is strictest-first and implemented in
`xtask/src/boot_manifest.rs::select_anchor`:

1. `AGENTOS_MOK_SIGNING_KEY` set → `AOS_ANCHOR_MOK`. The vendor key
   (`AGENTOS_BUNDLE_SIGNING_KEY`) is **optional** for this tier — if also
   set, vendor-signed images verify too; if not, the machine owner stands
   alone and vendor-signed images (including agentOS's own release images)
   are refused unless the owner signs or counter-signs them. That is the
   intended meaning of "stand alone," not a defect.
2. Else `AGENTOS_BUNDLE_SIGNING_KEY` set alone → `AOS_ANCHOR_VENDOR`.
3. Else, with **neither** key set, `AGENTOS_TRUST_ANCHOR=none` given
   explicitly → `AOS_ANCHOR_NONE`. Verification still runs: every PD's ELF
   digest is still computed and compared against the manifest, and a
   mismatch is still reported loudly — but nothing gates boot. Development
   only; establishes nothing about image integrity.
4. Else (**no key, no opt-in**): **the build fails.** This is deliberate
   and is the one outcome this whole feature must never silently produce —
   a build with no key must never produce an image that looks verified but
   isn't. See the error message from `select_anchor` for exactly what to
   set.

The manifest is signed with the vendor key when one is configured (even
under the MOK tier, if both are present), otherwise with the MOK key,
otherwise — `AOS_ANCHOR_NONE` only — with this in-tree dev seed, purely so
the manifest is a well-formed, checkable blob for the per-PD digest
machinery; it carries no trust claim at that tier.

### MOK enrolment: what actually exists today

`AGENTOS_MOK_SIGNING_KEY` is the machine-owner key's enrolment mechanism on
this platform **today, and it is build-time-provisioned, not a persistent
runtime enrolment flow.** Pointing that env var at a 32-byte Ed25519 seed
file at build time is the entire enrolment story: there is no on-target
storage a running system writes to when an owner enrols a key with physical
presence (the way a real UEFI MokManager / shim deployment works), and this
code does not pretend otherwise. A real runtime enrolment flow would need
writable, attested storage reachable at boot (e.g. a dedicated flash region
or TPM NV index) that this platform does not have yet — build-time
provisioning is the honest stand-in until that exists, not a stub that
imitates persistence.

## Using your own key instead

Set `AGENTOS_BUNDLE_SIGNING_KEY` to the path of your own 32-byte raw Ed25519
seed before building (e.g. `openssl rand -out my_key.seed 32`) to sign and
verify under `AOS_ANCHOR_VENDOR`. Set `AGENTOS_MOK_SIGNING_KEY` instead (or
in addition) to sign/verify under `AOS_ANCHOR_MOK`. When set, `xtask
gen-pd-bundle` reads the corresponding file(s) and compiles the *matching*
public key(s) into the root task — the dev key and its warnings are not
used at all once a real key is configured for the slot(s) your build
selects. See `xtask/src/boot_manifest.rs::select_anchor` for the exact
logic.
