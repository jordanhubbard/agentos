# RISC-V architecture parity

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Bring `riscv64` from "does not compile" to "boots the full platform under QEMU with a real PD set, proven in CI" — and make the three-architecture claim honest about what each one actually does.

**Spec:** this file plus `.sdd/arch-parity-audit.md` and `.sdd/riscv-guest-feasibility.md`, both evidence-cited.

## The honest scope, decided up front

The request was "boot all the way into guest operating systems on all 3 architectures." **That is not reachable on riscv64**, and no work in this repository changes it:

- Upstream seL4 has **no** RISC-V hypervisor extension — no `seL4_RISCV_VCPUObject`, no `vcpu.c`, no Kconfig, in any release 13.0.0–16.0.0 or on master. Verified by reading the tree, not release notes.
- The working code lives in a fork stack (`Ivan-Velickovic/seL4@microkit_riscv_he`, `Ivan-Velickovic/microkit@riscv_he`, `au-ts/libvmm@riscv`) targeting ratified H v1.0. The libvmm maintainer's own issue (au-ts/libvmm#246, May 2026) names seL4 as the blocker and says the Microkit side must be **re-done** on capDL, not rebased. No upstream PR or RFC exists.
- Enabling the H-extension **forfeits the RV64 binary-verification result**. If riscv64's value in the lineup is "the verified architecture," virtualization destroys exactly that.

So this plan delivers **platform parity, not guest parity**, and the resulting claim is "three architectures boot the platform; aarch64 and x86_64 run guests." Guest-on-riscv64 is tracked separately as upstream-gated work. Do not pull a forked kernel into `sdk-candidate` — that pipeline rebuilds pinned commits plus a 77-line patch and checks kernel hashes.

QEMU is not a blocker: `-cpu rv64` already reports `rv64imafdch`, H included, on the local QEMU 11.1.1.

## What is actually broken, in dependency order

Measured, not inferred:

1. **Build** — one compile error in the whole tree (`ut_alloc.c:167`, duplicate case label: `boot_info.h` maps both `seL4_ARCH_IntermediatePTObject` and `seL4_ARM_VSpaceObject` to `seL4_RISCV_PageTableObject`). 52 PD ELFs build clean behind it. **Fix exists**, written on `trust-t10-anchor-tiers`.
2. **Nothing boots** — `agentos.img` is a custom `"AGENTOS\0"` container. aarch64 boots `kernel/loader/`'s `loader.elf` (an ELF QEMU can load) which copies seL4 + root task to their physical addresses and jumps; x86_64 uses multiboot. **riscv passes the raw container to `-kernel`**, so OpenSBI starts, prints its banner, and nothing executes. Confirmed by running it: 65 lines of OpenSBI, then silence to timeout.
3. **No root-task entry point** — `ROOT_TASK_ASM_OBJS` is empty for riscv and there is no `start_riscv64.S`.
4. **No PD-loading mechanism** — `main.c:2976-2979` claims "On RISC-V PDs load via the seL4 extra BootInfo path." That is **false**: `grep AGENTOS_BOOTINFO_HEADER_ELF` finds the `#define` and two *consumer* loops, and **no producer anywhere**. All PDs would fail to spawn. Fixing it by extending the bundle predicate also restores the signed-manifest check riscv currently compiles out.
5. **CI cannot see any of this** — `tools/sdk/candidate.mk` builds `--boards qemu_virt_aarch64,x86_64_generic,x86_64_generic_vtx`; no riscv64 kernel is produced at all. The only `riscv` string under `.github/` is a `hurd-services-build` matrix entry that never uses its `board` variable and whose `make -n` hardcodes `ARCH=aarch64` — decorative coverage that creates a false impression.

## Global Constraints

- Language policy: C, Rust, Assembly only. `make policy-check` enforces it.
- Local target verification requires `SEL4_SDK_VERSION=2.1.0`; CI's `os-claim-gate` uses the pinned SDK artifact.
- **Do not change the aarch64 or x86_64 default PD sets.** `make test-authority` and `make test-inspect` assert exact counts (aarch64: 15 PDs).
- Do not modify anything under `services/legacy-pds/`.
- Never put a forked seL4 or Microkit into `tools/sdk/`.

## Review Focus

1. **A riscv64 "boot proof" that proves nothing.** x86_64 already has this failure in the tree: the CI gate boots `x86_64_generic` to `[rt] boot complete` with **zero PDs**, because the whole descriptor sits inside `#if defined(AGENTOS_X86_VTX)`. A riscv64 test asserting only a boot marker would repeat it. Every new boot proof must assert a PD count. → Tasks 2, 4, 5.
2. **Claiming guest support on riscv64.** The `__riscv` arm of `guest_vmm.c` is a process-in-PD `jalr` with no vCPU, no stage-2 translation and `_guest_kernel_image` permanently NULL. It is not a guest and `docs/TCB.md` must not imply it is. → Task 4.
3. **Breaking aarch64/x86_64 while generalising.** Every predicate widened for riscv is a chance to change behaviour on a working architecture. → all tasks.
4. **Re-introducing arch-specific code that compiles on one target only.** The `ut_alloc.c` bug is the template: correct on aarch64, a hard error elsewhere, invisible because nothing built it. → Task 1's CI job is the backstop; it must actually fail when riscv64 breaks.
5. **Verification silently weaker on riscv64.** riscv compiles out the signed-manifest check today. If PDs become loadable there, T3/T10 verification must apply, or `docs/TCB.md` must say plainly that it does not. → Task 3.

---

### Task 1: Stop the rot — build riscv64, and prove CI catches it

**Files:** `kernel/agentos-root-task/src/ut_alloc.c`, `tools/sdk/candidate.mk`, `.github/workflows/sdk-candidate.yml`, `.github/workflows/ci.yml`, `tools/sdk/cr2-kernels.sha256`.

This task buys the ability to measure everything after it. It adds no features.

- [ ] **Step 1:** Take the `ut_alloc.c` fix already written on `trust-t10-anchor-tiers` (commit `38000d60`): `#if !defined(__riscv)` around the `seL4_ARM_VSpaceObject` case label, with its existing comment explaining that both names expand to `seL4_RISCV_PageTableObject` on RISC-V and are genuinely distinct on AArch64/x86_64. If PR #297 has already merged, this is present — verify rather than duplicate.
- [ ] **Step 2:** Confirm `make build TARGET_ARCH=riscv64 SEL4_SDK_VERSION=2.1.0` exits 0 and reports `arch=riscv64`. Record the PD count it prints.
- [ ] **Step 3:** Add `qemu_virt_riscv64` to `--boards` in `tools/sdk/candidate.mk`, add a riscv64 cross-GCC to the `sdk-candidate` workflow, and extend `tools/sdk/cr2-kernels.sha256` with the resulting kernel hash. The SDK pipeline rebuilds pinned commits and checks hashes — do not disturb that discipline.
- [ ] **Step 4:** Add a real CI job that **builds the riscv64 root task**. Not a syntax check: it must compile and link `root_task.elf` and fail the workflow when it does not.
- [ ] **Step 5: Prove the job is not vacuous.** Temporarily revert the `ut_alloc.c` fix, confirm the new job **fails**, restore it, confirm it passes. Paste all three outputs. A CI job that cannot fail is the thing this task exists to prevent.
- [ ] **Step 6:** Fix or delete the decorative riscv entry in `hurd-services-build` (`ci.yml:561`) that never uses its `board` variable. Leaving a job that looks like riscv coverage next to one that is, is worse than having neither.
- [ ] **Step 7:** `make test-host`, `make policy-check`, aarch64 boot test, `make test-authority`, `make test-inspect`. Commit.

---

### Task 2: Make riscv64 boot — loader and entry point

**Files:** create `kernel/loader/` riscv64 arm and `kernel/agentos-root-task/src/start_riscv64.S`; modify `kernel/loader/Makefile`, `kernel/agentos-root-task/Makefile`, `xtask/src/cmd_test.rs`.

**Interfaces:** the loader consumes the same `"AGENTOS\0"` container aarch64 uses; read `kernel/loader/` for the header format rather than inventing one.

- [ ] **Step 1:** Read the aarch64 loader end to end. It is the specification: QEMU loads it as an ELF, it reads `agentos.img` from a known physical address, copies the seL4 kernel and `root_task.elf` to their physical addresses, sets up page tables, enables the MMU, and jumps to the kernel. Write down what differs on RISC-V before writing code — OpenSBI has already left you in S-mode, paging is Sv39, and the boot HART/device-tree pointer arrive in `a0`/`a1`.
- [ ] **Step 2:** Write `start_riscv64.S`: the root task's `_start`, mirroring `start_aarch64.S`. Wire it into `ROOT_TASK_ASM_OBJS` for riscv.
- [ ] **Step 3:** Write the riscv64 loader arm. Keep `kernel/loader/Makefile`'s existing "skip silently on other arches" structure; replace the riscv skip with a real build.
- [ ] **Step 4:** Change the riscv QEMU invocation in `cmd_test.rs:3420` to boot the loader ELF and supply `agentos.img` as data, the way aarch64 does — not the container to `-kernel`.
- [ ] **Step 5:** Boot it. Target: the root task's first output (`[rt] UART mapped`) on the serial console. Paste the output.
- [ ] **Step 6:** `make test-host`, `make policy-check`, both other arches' boot tests unchanged. Commit.

---

### Task 3: Load PDs on riscv64

**Files:** `kernel/agentos-root-task/Makefile` (bundle predicate, ~line 1762), `kernel/agentos-root-task/src/main.c` (`AGENTOS_HAS_PD_BUNDLE`, ~line 446, and the false claim at 2976-2979).

- [ ] **Step 1:** Delete the comment at `main.c:2976-2979` claiming PDs load on RISC-V via the seL4 extra BootInfo path. There is no producer; the claim is false and it is why nobody noticed PDs cannot start there.
- [ ] **Step 2:** Extend `AGENTOS_HAS_PD_BUNDLE` to riscv64 so PDs are embedded and loaded exactly as on aarch64. Check every site the predicate guards — T10 added several, including the trust-anchor banner and `boot_verify_manifest()`.
- [ ] **Step 3:** This necessarily turns on signed-manifest verification for riscv64. That is the point: it must not be weaker there. Confirm `make build TARGET_ARCH=riscv64` produces a signed bundle and the boot refuses a tampered PD.
- [ ] **Step 4:** Update `docs/TCB.md` where it describes bundle-less architectures — after this, riscv64 is no longer one, and T10's `AOS_ANCHOR_UNVERIFIED` path should no longer be reachable there. Verify the sentinel's host tests still pass and that nothing now reports `UNVERIFIED` on a bundled arch.
- [ ] **Step 5:** Boot to `agentOS boot complete` with a non-zero PD count. Paste the output and the count.
- [ ] **Step 6:** Full verification on all three arches. Commit.

---

### Task 4: A real riscv64 PD set, and an honest guest statement

**Files:** `kernel/agentos-root-task/src/system_desc_riscv64.c`, `kernel/agentos-root-task/agentos.toml` / `boards/qemu-riscv64/`, `docs/TCB.md`, `xtask/src/cmd_test.rs`, `Makefile`, `.github/workflows/ci.yml`.

riscv64's descriptor is the pre-virtualizer topology plus HURD-era PDs (`event_bus`, `irq_pd`, `timer_pd`, `controller`, `init_agent`, `agentfs`, `vibe_engine`, `vfs_server`, `net_server`, `framebuffer_pd`) and is missing all three virtualizers. It never tracked the aarch64 evolution.

- [ ] **Step 1:** Bring the descriptor in line with the aarch64 topology: driver PDs own a device class, virtualizers mux it. Add `net_virt`, `blk_virt`, `serial_virt`. Drop the museum PDs — `CLAUDE.md` forbids new ones and these are the old ones.
- [ ] **Step 2:** Add `make test-riscv64` asserting boot **and an exact PD count**, mirroring `test-inspect`'s discipline. A marker-only assertion repeats the x86_64 zero-PD mistake.
- [ ] **Step 3:** Add it to `gate` **and** as an explicit `os-claim-gate` CI step. No CI job invokes `make gate`.
- [ ] **Step 4: `docs/TCB.md` — state the guest position plainly.** riscv64 boots the platform and runs native PDs; it does **not** run guest operating systems, because upstream seL4 has no RISC-V hypervisor extension. Name the fork stack and the verification trade-off so the next reader does not have to rediscover them. Do not describe `guest_vmm.c`'s `__riscv` arm as guest support — it is a same-privilege `jalr` with `_guest_kernel_image` permanently NULL.
- [ ] **Step 5:** Full verification on all three arches, plus the new target. Commit.

---

### Task 5: Make x86_64's claim honest too

**Files:** `kernel/agentos-root-task/src/system_desc_x86_64.c`, `xtask/src/cmd_test.rs:5253`, `.github/workflows/ci.yml`, `docs/TCB.md`.

Found during the audit and worth its own task: **the CI gate boots `x86_64_generic` to `[rt] boot complete` with zero PDs**, because the entire descriptor is inside `#if defined(AGENTOS_X86_VTX)` (`system_desc_x86_64.c:14,257`). The "Dual-arch OS-claim gate" therefore proves, on x86_64, that the root task started and nothing else. Meanwhile the *working* x86_64 guest path — a non-libvmm VMX/EPT firmware VMM that boots Debian amd64 to login and SSH — has **no CI coverage at all** (`grep vtx .github/workflows/*.yml` → no hits).

- [ ] **Step 1:** Decide and implement one of: give `x86_64_generic` a non-empty PD set, or stop describing it as a boot proof. Record which and why. Do not leave a zero-PD boot labelled as a gate.
- [ ] **Step 2:** Assert a PD count in the x86_64 boot test, so a zero-PD image can never pass again.
- [ ] **Step 3:** Add CI coverage for the VTX guest path (`gate-x86_64-vtx` → `-firmware-reset` → `-linux-login`), or state explicitly in `docs/TCB.md` that x86_64 guest support is real but unautomated and name what a runner would need (Linux host, `/dev/kvm`, nested VMX). An honest "unproven in CI" beats a silent gap.
- [ ] **Step 4:** Full verification. Commit.
