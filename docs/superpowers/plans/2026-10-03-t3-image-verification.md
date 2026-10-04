# T3 — Protection-Domain Image Verification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Verify every protection-domain image before the root task spawns it, so a modified image refuses to boot instead of booting silently.

**Architecture:** The build emits a **signed manifest** alongside the PD bundle: one entry per PD carrying its name and the SHA-256 of its ELF bytes, followed by a single Ed25519 signature over the manifest body. At boot the root task verifies that one signature against a public key fixed at build time, then checks each PD's digest immediately before spawning it. One signature keeps the expensive operation to a single Ed25519 verify; per-PD digests let a failure name the image that failed.

**Tech Stack:** C11 freestanding (seL4/Microkit), existing `libs/pd-support/ed25519_verify.c` and `libs/pd-support/sha256_mini.c`, Rust `xtask` for the build-side signer, `cargo xtask qemu-test` for target proof.

**Spec:** [`docs/superpowers/specs/2026-10-03-trust-delegation-design.md`](../specs/2026-10-03-trust-delegation-design.md) — section "T3 — Image verification before spawn".

## What this establishes, and what it does not

**Establishes:** no protection domain starts unless its ELF bytes match a digest covered by a signature made with the vendor's private key, which never exists on the board. This constrains every adversary who can modify an image but not replace the boot chain — remote compromise, tampering in the supply path after signing, and partial modification of a resident image.

**Does NOT establish resistance to the local operator.** The threat model has the operator untrusted with physical access. A public key shipped in the image can be replaced along with the image it validates, so an operator who controls the boot medium can substitute the whole bundle, key included. Software signing cannot prevent that; only a hardware anchor can — RPi5 OTP-fused signed boot, or Intel Secure Boot / PTT — and those are unconfirmed for the target units (spec open question 5).

The component is built identically either way. Only the claim changes. **Do not write a claim that implies operator resistance.**

**This is not a measured-boot chain.** No PCR, no attestation, no quote. That is T8 and it is gated on hardware.

## Global Constraints

- Language policy: C, Rust, or Assembly only. `make policy-check` enforces it.
- Host tests under `-DAGENTOS_TEST_HOST` are a compile/logic pre-filter only. Only a booted image asserted by an automated test proves target behaviour.
- **Target verification in this environment requires `SEL4_SDK_VERSION=2.1.0`.** The default pin is built from upstream clones absent here. CI's `os-claim-gate` job runs under the verified SDK artifact.
- Do not extend museum components. In particular `services/legacy-pds/verify.c` has a `VIBE_VERIFY_MODE` that defaults to *warn but allow*. **Do not reuse it and do not carry that pattern forward** — a verification path that can be configured off is reported as present and behaves as absent. Verification here is unconditional.
- `libs/pd-support/ed25519_verify.c` provides `int ed25519_verify(const uint8_t sig[64], const uint8_t *msg, uint32_t msg_len, const uint8_t pubkey[32])` returning 0 on valid. `libs/pd-support/sha256_mini.c` provides SHA-256; read `kernel/agentos-root-task/include/sha256_mini.h` for its exact API before using it.
- The bundle header is `agentos_bundle_hdr_t` (magic `0x4147454E544F5300`, `version`, `num_pds`, offsets) and entries are `agentos_bundle_pd_entry_t { char name[48]; uint32_t elf_off; uint32_t elf_len; uint8_t priority; uint8_t _pad[7]; }`, defined in `kernel/agentos-root-task/src/main.c` and written by `xtask/src/cmd_gen_pd_bundle.rs`. The 7 pad bytes cannot hold a signature — hence a separate manifest.

## Review Focus

1. **Verification that can be switched off.** A build flag, env var, or `#ifdef` that skips verification makes the whole feature a lie. There must be no such path. → Task 2, Task 3.
2. **Fail-open on a malformed manifest.** A truncated, absent, or wrong-version manifest must refuse boot, not skip verification. Treating "no manifest" as "nothing to check" is the classic form of this bug. → Task 2.
3. **The digest covers the wrong bytes.** If the digest is computed over a different range than the loader actually spawns, verification passes while the real image is unchecked. The digest must cover exactly `[elf_off, elf_off+elf_len)` as the loader reads it. → Task 2, Task 3.
4. **A vacuous target proof.** A probe asserting "boot succeeds" proves nothing about verification. The proof must tamper with an image and observe refusal. → Task 3.
5. **Overclaiming operator resistance.** See above. → Task 3.

---

### Task 1: Manifest format and host-testable verifier

**Files:**
- Create: `kernel/agentos-root-task/include/boot_manifest.h`
- Create: `kernel/agentos-root-task/src/boot_manifest.c`
- Test: `tests/test_boot_manifest.c`

**Interfaces:**
- Produces: `AOS_BOOT_MANIFEST_MAGIC`; `AOS_BOOT_MANIFEST_VERSION` (1); `AOS_BOOT_MANIFEST_MAX_PDS` (32); `aos_boot_manifest_entry_t { char name[48]; uint8_t sha256[32]; }`; `aos_boot_manifest_hdr_t { uint64_t magic; uint32_t version; uint32_t count; uint8_t reserved[16]; }`; `int aos_boot_manifest_validate(const uint8_t *blob, uint32_t len)`; `const aos_boot_manifest_entry_t *aos_boot_manifest_find(const uint8_t *blob, uint32_t len, const char *name)`.

Layout: header, then `count` entries, then a 64-byte Ed25519 signature over everything preceding it.

- [ ] **Step 1: Write the failing test**

Create `tests/test_boot_manifest.c` asserting, at minimum:
- a well-formed blob validates;
- a blob shorter than the header is rejected;
- a blob whose declared `count` exceeds what `len` can hold is rejected (**this is the truncation case — it must not read past the buffer**);
- a wrong magic is rejected;
- a wrong version is rejected;
- `count == 0` is rejected — an empty manifest must not be treated as "nothing to verify";
- `aos_boot_manifest_find` returns the right entry for a present name and NULL for an absent one;
- a name occupying all 48 bytes without a NUL terminator does not over-read.

Write the assertions concretely with a locally-constructed blob; do not rely on a fixture file.

- [ ] **Step 2: Run it and confirm it fails** — missing header.

- [ ] **Step 3: Implement header and validator**

`aos_boot_manifest_validate` must perform every bounds check before any field is trusted, and must return a distinct error for each rejection reason so the boot diagnostic can say *why*. It must not assume NUL-terminated names.

- [ ] **Step 4: Run the test and confirm it passes.**

- [ ] **Step 5: Wire into the host suite**, modelled on `test-remoteos-client-host` (`Makefile:921-928`).

- [ ] **Step 6: `make test-host` and `make policy-check`** — both pass.

- [ ] **Step 7: Commit.**

---

### Task 2: Build-side signing and boot-time verification

**Files:**
- Modify: `xtask/src/cmd_gen_pd_bundle.rs` (emit the manifest), `xtask/src/lib.rs`
- Create: a key-material module for the build (dev key when none supplied)
- Modify: `kernel/agentos-root-task/src/main.c` (verify before spawn)
- Modify: `kernel/agentos-root-task/Makefile`, root linker script (new section), `tools/ld/root_task.ld`

- [ ] **Step 1: Emit the manifest at build time**

In `cmd_gen_pd_bundle.rs`, after computing each PD's ELF bytes, build the manifest: one entry per PD with its bare stem name and the SHA-256 of exactly the bytes placed at `[elf_off, elf_off+elf_len)`. Sign the header+entries with Ed25519 and append the 64-byte signature.

The signing key comes from `AGENTOS_BUNDLE_SIGNING_KEY` (a path to a 32-byte seed). When unset, use a **well-known development key** committed in-tree, and print a clear warning that the image is development-signed. The matching public key must be compiled into the root task.

Mirror whatever conventions `tools/sign-wasm` already uses for key handling rather than inventing new ones — read it first.

- [ ] **Step 2: Place the manifest where the root task can read it**

Add a `.pd_manifest` section with `__pd_manifest_start` / `__pd_manifest_end` symbols, exactly as `.pd_bundle` already does (`main.c:405-415`, `tools/ld/root_task.ld`), and have the build `objcopy --update-section` it in.

- [ ] **Step 3: Verify at boot, before any spawn**

In `main.c`, before the PD spawn loop:
- validate the manifest blob with `aos_boot_manifest_validate`;
- verify its Ed25519 signature against the compiled-in public key;
- on any failure, print a `[rt]` diagnostic naming the reason and **refuse to start any PD**.

Then, immediately before spawning each PD, compute the SHA-256 of its ELF bytes **as the loader reads them** and compare against the manifest entry found by name. A mismatch, or a PD with no manifest entry, refuses boot and names the image.

**There must be no way to skip this.** No `#ifdef`, no environment variable, no build flag that disables it. If you believe one is needed for a test variant, STOP and report rather than adding it — the ability to switch verification off is the defect this task exists to avoid.

An absent or empty `.pd_manifest` section is a **failure**, not a skip.

- [ ] **Step 4: Build and boot**

`make test-host && make test TARGET_ARCH=aarch64 GUEST_OS=none SEL4_SDK_VERSION=2.1.0` — both pass; boot reaches `agentOS boot complete`.

If the boot is now meaningfully slower, report the measured difference. Hashing ~6 MB of ELF at boot is expected to be perceptible; a large regression is worth knowing about.

- [ ] **Step 5: Commit.**

---

### Task 3: Target proof and documentation

**Files:**
- Modify: `xtask/src/lib.rs`, `xtask/src/cmd_test.rs` (tamper probe)
- Modify: `Makefile` (`test-image-verify`, add to `gate`), `.github/workflows/ci.yml`
- Modify: `docs/TCB.md`

- [ ] **Step 1: Probe 1 — an unmodified image boots**

Assert the normal boot still reaches `agentOS boot complete` with verification active. This is the control.

- [ ] **Step 2: Probe 2 — a tampered image refuses to boot**

This is the proof. After building the image, **flip a byte inside one PD's ELF region in the built image file**, boot it, and assert that the root task refuses, emits its diagnostic, and **names the tampered PD**. Assert the absence of `agentOS boot complete`.

Choose the tampered byte inside `[elf_off, elf_off+elf_len)` of a known PD, derived from the bundle header rather than a hardcoded file offset, so the probe does not silently stop testing anything when the layout shifts.

- [ ] **Step 3: Probe 3 — a stripped manifest refuses to boot**

Zero the `.pd_manifest` section in the built image and assert boot is refused. This pins Review Focus item 2: absent manifest must fail, not skip.

- [ ] **Step 4: Makefile target and CI step**

Add `test-image-verify` running all three probes, add it to `gate`, **and** add a `GATE — make test-image-verify` step to the `os-claim-gate` job in `.github/workflows/ci.yml` beside the existing gate steps. No CI job invokes `make gate`, so a proof reachable only through that target never runs.

- [ ] **Step 5: Prove probe 2 is not vacuous**

Temporarily make the digest comparison always succeed, rebuild, and confirm probe 2 **FAILS** — the tampered image boots. Restore and confirm it passes. Paste all three outputs.

- [ ] **Step 6: Update `docs/TCB.md`**

State the boundary precisely:

```markdown
The root task verifies every protection-domain image before spawning it. The
build emits a manifest of per-PD SHA-256 digests signed once with Ed25519; root
verifies that signature against a public key fixed at build time, then checks
each PD's digest immediately before spawn. Verification is unconditional: there
is no build flag, environment variable or configuration that disables it, and an
absent or malformed manifest refuses boot rather than skipping the check.
`make test-image-verify` boots an unmodified image, a byte-tampered image, and
an image with its manifest stripped, requiring the latter two to be refused with
the tampered image named.

Scope: this constrains every adversary who can modify an image but not replace
the boot chain. It does NOT establish resistance to the local operator, who is
untrusted under the platform threat model and has physical access: a public key
shipped in the image can be replaced along with the image it validates. Only a
hardware anchor — OTP-fused signed boot or a firmware TPM — would change that,
and none is confirmed for the target boards. This is not a measured-boot chain
and produces no attestation.
```

- [ ] **Step 7: Full verification** — `make test-host`, `make policy-check`, the aarch64 boot test, `make test-image-verify`, `make test-inspect`, `make test-authority`, `make test-cc-envelope`, all under `SEL4_SDK_VERSION=2.1.0` where they touch the target. Report that `make gate` was not run.

- [ ] **Step 8: Commit.**

---

## Notes for the implementer

**The one thing that must not happen is a disableable check.** `services/legacy-pds/verify.c` shows the failure mode: a `VIBE_VERIFY_MODE` that defaults to warn-but-allow, so the code reads as if verification exists while the system behaves as if it does not. Do not reproduce it. If something seems to require an escape hatch, that is a design question for the controller, not an implementation decision.

**Be precise about what the digest covers.** If the bytes hashed at build time and the bytes spawned at boot can ever differ — padding, alignment, a relocation — verification passes while the real image is unchecked. Derive both from the same offsets.

**Do not claim operator resistance.** The threat model is explicit that the operator is untrusted and holds the medium. This work is still worth doing; just describe it accurately.
