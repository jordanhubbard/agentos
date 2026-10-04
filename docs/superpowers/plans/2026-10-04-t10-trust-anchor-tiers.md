# T10 — Trust anchor tiers (MOK model)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Let image verification work end to end across three trust anchors — none (development), a machine-owner-enrolled key, and a built-in vendor key — with the hardware anchor stubbed as a real but unimplemented arm, so TPM/OTP support later is a new key source rather than a new design.

**Extends T3** (`docs/TCB.md`, "Protection-domain image verification"), which is merged and currently verifies against a single compiled-in key.

## Why this exists, and the honest consequence

Production hardware with a usable key store does not exist yet, and blocking iteration on that would be the wrong trade. Linux solved the same problem with shim/MOK: a built-in vendor certificate plus a Machine Owner Key the owner enrols with physical presence.

**This contradicts a claim T3 currently makes and that claim must be amended.** `docs/TCB.md` says today:

> On those targets it is unconditional: no build flag, environment variable or configuration disables it.

After this task that is false, and leaving it would be exactly the overclaim this project treats most seriously. It must be rewritten to state the tier model and what each tier establishes.

**What makes a tiered anchor safe, where `VIBE_VERIFY_MODE` was not.** The museum `services/legacy-pds/verify.c` has a mode that defaults to warn-but-allow and is invisible at runtime, so the code reads as if verification exists while the system behaves as if it does not. The difference here is not a matter of degree:

1. **Development mode does not skip verification — it verifies and reports without gating.** Digests are still computed and compared, and a mismatch is still announced. What changes is only whether boot stops.
2. **The tier is a recorded property of the artifact**, visible in boot output and in inspect data, not merely a build-time flag that vanishes.
3. **The permissive tier cannot be selected by accident.** It requires an explicit, named build input and produces a visibly marked image.

If any of those three is missing, this is `VIBE_VERIFY_MODE` with better branding. They are the acceptance criteria, not nice-to-haves.

## The tiers

| Tier | Key source | Gates boot? | Establishes |
| --- | --- | --- | --- |
| `AOS_ANCHOR_NONE` | none | **no** | Nothing about image integrity. Digests are computed and mismatches reported, so tampering is *visible*, but boot proceeds. Development only. |
| `AOS_ANCHOR_VENDOR` | compiled-in vendor public key | yes | What T3 establishes today: constrains every adversary who can modify an image but not replace the boot chain. |
| `AOS_ANCHOR_MOK` | machine-owner key from an enrolled store, **in addition to** the vendor key | yes | Same as vendor, plus images the owner signed. **Does not defend against the machine owner**, who can sign anything — see below. |
| `AOS_ANCHOR_HARDWARE` | OTP / firmware TPM | — | **Not implemented.** A real enum arm whose key source returns unavailable, so the shape exists without a false claim. |

**MOK narrows the guarantee, and `docs/TCB.md` must say so.** The platform threat model treats the local operator as untrusted with physical access. A key the owner enrolled does not constrain the owner — they can sign any image they like. MOK therefore protects against remote compromise and third-party tampering, not against the machine owner. That is worth having on legacy hardware; it is not the same claim as vendor-only signing, and conflating them would be an overclaim.

## Global Constraints

- Language policy: C, Rust, or Assembly only. `make policy-check` enforces it.
- **Target verification requires `SEL4_SDK_VERSION=2.1.0`** here; CI's `os-claim-gate` runs under the verified SDK artifact.
- Do not revive or imitate `services/legacy-pds/verify.c`. It is quarantined museum code and its mode is the anti-pattern this task must not reproduce.
- Do not modify anything under `services/legacy-pds/`; no `cap_broker`/`capstore`/`auth_server`.
- The default image's PD set must not change. `make test-authority` and `make test-inspect` both assert exact PD counts — run **both**.

## Review Focus

1. **A permissive tier reachable by default or by accident.** If an ordinary `make build` with no arguments produces a non-gating image, the feature is a loaded gun. The default must be the strictest tier the build inputs support. → Tasks 1, 2.
2. **A tier that is invisible at runtime.** If a running system cannot be asked which anchor it booted under, operators cannot tell a development box from a production one. → Tasks 2, 3.
3. **Development mode that stops verifying rather than stops gating.** Digests must still be computed and mismatches still reported in `AOS_ANCHOR_NONE`. Skipping the work entirely is the `VIBE_VERIFY_MODE` failure. → Tasks 2, 3.
4. **MOK described as defending against the owner.** It does not. → Task 3.
5. **The hardware arm claiming anything.** It is a stub. Any text implying TPM support exists is an overclaim. → Tasks 1, 3.

---

### Task 1: Anchor tier contract and key-source abstraction

**Files:** create `kernel/agentos-root-task/include/contracts/trust_anchor.h`, `libs/pd-support/trust_anchor.c`; test `tests/test_trust_anchor.c`.

Host-testable. No seL4 calls, no key material — this defines the tiers, the key-source interface, and the policy of which tier gates.

**Interfaces:** `AOS_ANCHOR_VERSION`; `aos_anchor_tier_t { AOS_ANCHOR_NONE=0, AOS_ANCHOR_VENDOR=1, AOS_ANCHOR_MOK=2, AOS_ANCHOR_HARDWARE=3 }`; `aos_anchor_key_t { uint8_t pubkey[32]; uint32_t present; }`; `aos_anchor_state_t { uint32_t version; uint32_t tier; aos_anchor_key_t vendor; aos_anchor_key_t mok; }`; `int aos_anchor_validate(const aos_anchor_state_t *)`; `int aos_anchor_gates_boot(const aos_anchor_state_t *)`; `const char *aos_anchor_tier_name(uint32_t tier)`.

- [ ] **Step 1:** Write `tests/test_trust_anchor.c` asserting:
  - `AOS_ANCHOR_VENDOR` with a present vendor key validates and **gates**;
  - `AOS_ANCHOR_MOK` requires **both** a vendor key and a MOK present — a MOK tier with no vendor key is rejected, because MOK is additive to the vendor root, not a replacement;
  - `AOS_ANCHOR_NONE` validates with **no** keys present and does **not** gate;
  - `AOS_ANCHOR_VENDOR` or `AOS_ANCHOR_MOK` with the corresponding key **absent** is rejected — a gating tier with no key is incoherent and must fail loudly, never silently degrade to non-gating;
  - `AOS_ANCHOR_HARDWARE` validates structurally but reports **not available**, and does not gate — it is a stub;
  - an unknown tier value is rejected;
  - a version mismatch is rejected before anything else;
  - NULL is rejected rather than dereferenced;
  - `aos_anchor_tier_name` returns a distinct, non-empty string for every tier and a safe value for an unknown one.
  Run it; it must fail.
- [ ] **Step 2:** Implement exactly those semantics. **`aos_anchor_gates_boot` must return false only for `AOS_ANCHOR_NONE` and `AOS_ANCHOR_HARDWARE`** — never as a fallback when a key is missing. A missing key in a gating tier is a validation failure, not a reason to proceed.
- [ ] **Step 3:** Run the test; it must pass. Wire `test-trust-anchor-host` into `make test-host`, modelled on `test-remoteos-client-host` (`Makefile:921-928`).
- [ ] **Step 4:** `make test-host`, `make policy-check`. Commit.

---

### Task 2: Wire the tiers into the boot verification path

**Files:** modify `xtask/src/boot_manifest.rs` and `xtask/src/cmd_gen_pd_bundle.rs` (emit the tier), `kernel/agentos-root-task/src/main.c` (consume it), the generated pubkey header, `kernel/agentos-root-task/keys/README.md`.

Today `render_pubkey_header()` emits `AOS_BOOT_MANIFEST_DEV_SIGNED` as a boolean driven by which key signed, consumed at `main.c:788`. Generalise that to a tier.

- [ ] **Step 1: Select the tier at build time, strictest-first.** `AGENTOS_BUNDLE_SIGNING_KEY` set → vendor. A MOK store configured in addition → MOK. Neither, **and** an explicit opt-in (e.g. `AGENTOS_TRUST_ANCHOR=none`) → none. **Without that explicit opt-in, a build with no key must fail rather than silently producing a non-gating image.** Defaulting to permissive is the single thing this task must not do.
- [ ] **Step 2: Emit the tier into the generated header**, replacing the boolean. Keep a dev-key indicator too — "signed with the in-tree dev key" and "no key at all" are different states and both need announcing.
- [ ] **Step 3: Consume it in `main.c`.** Announce the tier unmissably at boot, naming it: vendor, machine-owner, or **development (not gating)**. In `AOS_ANCHOR_NONE`, **still compute and compare every digest** and report mismatches loudly — then continue. Do not skip the comparison.
- [ ] **Step 4: Surface the tier in the inspect snapshot** so a running system can be asked which anchor it booted under. An operator must be able to tell a development box from a production one without reading the boot log.
- [ ] **Step 5: MOK enrolment.** Define where an enrolled MOK lives and how it is read at boot. Keep it minimal and honest: if persistent enrolment storage does not exist yet on this platform, implement the key-source interface and the verification path, and state plainly in the report and in `docs/TCB.md` that enrolment is build-time-provisioned for now. **Do not fake a persistence mechanism.**
- [ ] **Step 6:** `make test-host`, `make policy-check`, the aarch64 boot test, **and both** `make test-authority` and `make test-inspect`. Commit.

---

### Task 3: Target proof and documentation

**Files:** `xtask/src/lib.rs`, `xtask/src/cmd_test.rs`, `Makefile`, `.github/workflows/ci.yml`, `docs/TCB.md`.

- [ ] **Probe 1 — vendor tier gates.** An unmodified vendor-signed image boots; a byte-tampered one is refused and the PD named. (This is T3's existing behaviour under the new machinery — confirm it did not regress.)
- [ ] **Probe 2 — development tier reports but does not gate.** A tampered image under `AOS_ANCHOR_NONE` **boots to completion**, and the boot log carries both the development-tier announcement **and** the digest-mismatch report naming the PD. This is the probe that distinguishes "reports without enforcing" from "does not verify" — assert **both** the completion marker and the mismatch report.
- [ ] **Probe 3 — a gating tier with no key refuses to boot.** Construct the incoherent state and assert boot is refused, not silently downgraded.
- [ ] **Probe 4 — the tier is visible at runtime.** Read the inspect snapshot and assert it reports the tier the image was built with.
- [ ] **Step 5: Prove probe 2 is not vacuous.** Make the dev-tier path skip the digest comparison entirely, rebuild, and confirm probe 2 **fails** — the mismatch report is absent. Restore; confirm it passes. Paste all three outputs. Without this, "dev mode still verifies" is an untested claim.
- [ ] **Step 6: CI.** Add `test-trust-anchor` to `gate` **and** as a step in the `os-claim-gate` job. No CI job invokes `make gate`.
- [ ] **Step 7: Rewrite the T3 paragraph in `docs/TCB.md`.** Replace the "unconditional" sentence with the tier table and these statements:

```markdown
Image verification runs under one of three trust anchors, recorded in the image
and announced at boot. Under the vendor anchor (a public key fixed at build
time) and the machine-owner anchor (an enrolled key, additive to the vendor
key), a manifest or digest mismatch refuses boot. Under the development anchor,
digests are still computed and mismatches still reported, but boot proceeds —
it establishes nothing about image integrity and exists so the platform can be
iterated on before production key storage exists. The development anchor
requires an explicit build opt-in; a build with no key and no opt-in fails
rather than producing a non-gating image.

The machine-owner anchor does NOT defend against the machine owner, who can
sign any image they choose. It constrains remote compromise and third-party
tampering. Only the vendor anchor excludes the owner, and neither excludes an
adversary who can replace the boot chain — that needs a hardware root of trust.

A hardware anchor (OTP-fused key or firmware TPM) is defined as a key source
and is NOT implemented; it reports unavailable. No claim is made about TPM or
measured-boot support.
```

- [ ] **Step 8:** Full verification — `make test-host`, `make policy-check`, the aarch64 boot test, `make test-trust-anchor`, `make test-image-verify`, `make test-authority`, `make test-inspect`. Report that `make gate` was not run. Commit.

---

## Notes for the implementer

**The one unacceptable outcome is a build that silently produces a non-gating image.** Every other defect here is recoverable; that one hands someone a system they believe is verified and is not. If you are unsure whether a path can reach `AOS_ANCHOR_NONE` without the explicit opt-in, make it fail closed and say so.

**Development mode stops gating, not verifying.** If you find yourself writing `if (tier == NONE) return OK;` before the digest comparison, that is the museum's `VIBE_VERIFY_MODE` reappearing. Compute, compare, report — then decide whether to stop.

**Do not let the hardware arm imply support.** It exists so the later TPM work is a key source rather than a redesign. Until then it returns unavailable and claims nothing.
