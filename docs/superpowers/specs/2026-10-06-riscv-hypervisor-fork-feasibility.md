# Forking seL4 for RISC-V hypervisor support: a costed feasibility study

**Date:** 2026-10-06
**Question:** What would it actually take for agentOS to fork seL4, add RISC-V H-extension
support itself, and boot guest operating systems on riscv64?
**Status:** research and costing deliverable. No product code was written.

All measurements below were taken by cloning the repositories and running `git` against
them on 2026-10-06. Nothing is quoted from a GitHub UI summary. Where a number could not
be established it says so.

---

## Recommendation

**Do option (a): adopt the fork stack out-of-tree, behind a hard wall from `sdk-candidate`,
and start with the two-week spike in §6. Do not take option (b) — "fork and maintain
properly" — unless and until the spike succeeds *and* a named engineer owns riscv64
virtualisation for at least a year.**

Three facts drive that:

1. **The kernel delta is small and rebases almost cleanly.** The RISC-V H-extension work in
   `Ivan-Velickovic/seL4@microkit_riscv_he` is **37 files, 1,578 insertions, 25 deletions**,
   of which **zero** are generated. Applied to today's `seL4/seL4` master — 307 commits and
   ~16 months of drift later — it produces **7 conflict hunks in 6 files**, and every one of
   them is in build config or a small shared header. `vcpu.c`, `machine.h`, `vspace.c`,
   `hardware.c`, `c_traps.c` — all the actual hypervisor code — apply **clean**. The
   recurring rebase cost is therefore *not* the thing that should scare anyone.

2. **What should scare you is that 167 of those added lines are not behind the config
   flag**, and at least four of them change behaviour for the *non*-hypervisor RISC-V build
   that agentOS is currently standing up on PR #298. The worst is one line:
   `src/arch/riscv/object/objecttype.c` makes `Arch_getObjectSize(seL4_RISCV_PageTableObject)`
   return a hardcoded `14` — **unconditionally**, for every RISC-V build — quadrupling every
   page-table object from 4 KiB to 16 KiB. The branch is a working research prototype, not a
   clean opt-in feature, and the gap between those two is most of the engineering cost.

3. **Post-capDL, this is a four-repo problem, not three.** Microkit now builds its system
   image through `sel4-capdl-initializer` from `github.com/seL4/rust-sel4`
   (`Cargo.toml:14`), whose VCPU-binding path is gated
   `#[sel4_cfg(any(all(ARCH_ARM, ARM_HYPERVISOR_SUPPORT), all(ARCH_X86_64, VTX)))]`
   (`crates/sel4-capdl-initializer/src/initialize.rs:869`) and whose
   `crates/sel4/src/arch/riscv/object.rs` has no VCPU object type at all. The existing
   feasibility note (`.sdd/riscv-guest-feasibility.md`) lists seL4 + Microkit + libvmm.
   rust-sel4 is a fourth upstream, with different maintainers (Colias Group), and the
   fork's 97-line Microkit patch predates the capDL migration entirely.

A fourth fact is worth stating because the brief got it backwards, and it cuts the *other*
way. "The hypervisor config would be unverified, like AArch64 EL2" is false:
`configs/include/AARCH64_verified_include.cmake` sets `KernelArmHypervisorSupport ON`, so
**AArch64's verified configuration *is* the hypervisor configuration**, with real VCPU
theories in l4v. And the RV64 binary-verification result that H-extension support would
"forfeit" is pinned to platforms (`hifive`, `rocketchip`, …) and options (`KernelRiscvExtF OFF`,
`KernelRootCNodeSizeBits 19`, non-MCS) that **no Microkit riscv64 build matches** — so this
project does not hold that result today and would not be giving it up. The verification
objection to forking is weaker than it looks; the maintenance and ownership objections are
stronger. §4.

The decision this study is really being asked to support is "do we own a kernel fork." The
honest answer is that **the fork is cheap to carry and expensive to be responsible for.**
Carrying 1,578 lines of patch in `tools/sdk/patches/` costs on the order of a week a year.
Being the people who answer for a hypervisor kernel's correctness — on hardware you are
building, with no upstream, no proofs, no `sel4test` coverage and no second pair of eyes —
is a standing commitment of a different kind, and nothing in the measurements below makes
it smaller.

---

## 0. Five claims, kept apart

This project cares about not conflating these, so they are tabulated before anything else.

| Claim | RISC-V hypervisor on seL4 | Evidence |
| --- | --- | --- |
| **Exists on a branch** | **Yes.** `Ivan-Velickovic/seL4@microkit_riscv_he`, tip `6c4ffa2de` (2025-07-21), 14 `riscv-he:` commits. Ratified H v1.0. | Cloned and diffed; see §1 |
| **Works on QEMU** | **Reportedly yes**, by the author. Not reproduced by us. | au-ts/libvmm#246; not independently verified |
| **Works on hardware** | **Partially, reportedly.** "Basic Linux VM works on HiFive P550, but pass-through serial IRQ does not work for some reason." Not reproduced by us. | au-ts/libvmm#246 |
| **Is upstream** | **No.** No `src/arch/riscv/object/vcpu.c`, no `KernelRiscVHypervisorSupport`, zero hits for `RiscVHypervisor|RISCV_HYPERVISOR` anywhere on `seL4/seL4` master. No open PR. No RFC — RFC-1 … RFC-27 contain none. `docs.sel4.systems/Hardware/` says plainly: *"Support for the hypervisor extension is yet to be mainlined."* | Verified against a fresh clone and the RFC repo |
| **Is verified** | **No**, and cannot be without ≥2 person-years of new Isabelle work. `spec/abstract/RISCV64/Hypervisor_A.thy` in l4v is a one-line no-op stub; there is no RISC-V `VCPU_A.thy`. | §4 |

Note the fifth row's asymmetry with the fourth: upstream seL4 explicitly *does* accept
unverified configurations. `seL4/seL4` `CONTRIBUTING.md` says contributions to master should
"either be invisible to the proof in l4v, such as comments, documentation, style,
**unverified platform**, etc, or they should come with proof updates to `l4v`." Being
unverified is not what is keeping this out of upstream.

---

## 1. How big is the actual delta?

### 1.1 Finding the right baseline

`microkit_riscv_he` is **not** branched from `seL4/seL4` master. Its merge-base with master
is `5e6f7c2bda` (2025-05-23), but between that and the hypervisor work sit nine
`Merge branch 'master' into microkit` commits plus three microkit-prerequisite commits. The
branch descends from seL4's old long-lived `microkit` branch, which no longer exists upstream.

So there are two deltas, and conflating them inflates the number by ~60%:

| Measured from | Files | Insertions | Deletions | What it is |
| --- | ---: | ---: | ---: | --- |
| master merge-base `5e6f7c2bda` → tip | 47 | 1,669 | 53 | HE work **+** 3 leftover microkit boot-protocol commits |
| `3aafe9e0b` (last microkit merge) → tip | **37** | **1,578** | **25** | **the hypervisor work, and only that** |

The 37-file figure is the one to cost. Two of the three extra commits are already upstream
or obsolete (`92f0f3ab2 Remove python2 support` landed upstream as `4d7fc32a2`).

For scale: the fork's 1,578 lines sit between seL4's two existing hypervisor ports —
AArch64 is `src/arch/arm/object/vcpu.c` 610 lines + `vcpu.h` 217 + GICv2/v3 virtualisation
723, with 348 `CONFIG_ARM_HYPERVISOR_SUPPORT` occurrences tree-wide; x86 VT-x is
`src/arch/x86/object/vcpu.c` 1,633 + `vcpu.h` 432. The RISC-V delta is *smaller than both*,
which is itself a signal about completeness, not efficiency (§1.4).

### 1.2 Where the lines are

Full per-file breakdown of `3aafe9e0b..6c4ffa2de`:

| Subsystem | Files | +lines | Notes |
| --- | ---: | ---: | --- |
| **vCPU object** | `src/arch/riscv/object/vcpu.c` (new), `include/arch/riscv/arch/object/vcpu.h` (new) | 476 + 52 | 30 functions: save/restore of 10 VS-mode CSRs, `vcpu_switch`, `handleVCPUFault`, three invocations |
| **CSR / machine abstraction** | `include/arch/riscv/arch/machine.h` | 419 | H-extension CSR accessors (`hstatus`, `hedeleg`, `hideleg`, `hvip`, `htval`, `htinst`, `hgatp`, `vsstatus`…), `hfence`, `setKernelVSpaceRoot` |
| **Interrupt delivery + virtual timers** | `src/arch/riscv/machine/hardware.c`, `include/arch/riscv/arch/smp/ipi.h`, `include/drivers/irq/riscv_plic0.h` | 135+10+5 | A software vtimer multiplexer over the single S-mode timer; **no vAIA, no in-kernel vPLIC** |
| **Stage-2 (G-stage) translation** | `src/arch/riscv/kernel/vspace.c`, `include/arch/riscv/arch/kernel/vspace.h` | 88+3 | `hgatp` write via `setVSpaceRoot`; guest-fault decode from `htval`/`htinst` |
| **Trap/fault plumbing** | `src/arch/riscv/c_traps.c`, `src/arch/riscv/api/faults.c` | 22+13 | three new guest-page-fault causes, `RISCVEnvHypCall`, `RISCVVirtualInstruction` |
| **Capability / object-type plumbing** | `structures.bf`, `objecttype.c`, `structures.h`, `structures_64.bf` | 71+47+22+3 | new `vcpu_cap`, `s2_root_page_table_cap`, `vmid_control_cap`, `vmid_pool_cap`, `hgatp` block, `VCPUFault` block |
| **libsel4 bindings** | `object-api-arch.xml`, `constants.h`, `types.bf`, `objecttype.h`, `types.h`, `shared_types.bf`, `syscall_stub_gen.py` | 36+48+12+3+1+6+2 | 3 invocations; `seL4_VCPUReg_*` enum (10 regs); `seL4_RISCV_VCPUObject` |
| **Build config** | `config.cmake`, `CMakeLists.txt`, `qemu-riscv-virt/config.cmake`, `platform_gen.h.in` | 16+6+5+6 | `KernelRiscVHypervisorSupport`, `KernelRiscVNumVTimers`, appends `h` to `-march` |
| **Misc (FPU, thread, statedata, fastpath)** | 7 files | 30 | |

**Mechanical vs genuine:** there are **no generated files** in the delta. The `.bf` and
`.xml` files are *inputs* to seL4's generators, not outputs. The two `syscall_stub_gen.py`
lines are the only tooling change. Treat the whole 1,578 lines as hand-written kernel work.

### 1.3 The hard parts, with file:line

**(a) Enabling H changes the execution mode of *every* PD, not just VMs.**
`include/arch/riscv/arch/machine.h:649-667`: with `CONFIG_RISCV_HYPERVISOR_SUPPORT`,
`setVSpaceRoot()` writes **`hgatp`**, not `satp`. Every seL4 VSpace becomes a G-stage
(stage-2) translation and every user thread runs in VS-mode. The kernel keeps stage-1 for
itself via a new `setKernelVSpaceRoot()` (`machine.h:671`), called from
`src/arch/riscv/kernel/vspace.c:307`. There is no per-TCB "this one is a guest" mode switch.

Consequence for agentOS specifically: there is no "turn on virtualisation for the VMM PDs
only" build. A riscv64 hypervisor SDK is a **different system** from the native-PD riscv64
system PR #298 is building — different page-table sizes, different ASID geometry, different
fault paths. You would be qualifying two riscv64 kernels, not adding a feature to one.

**(b) VMID management is not implemented; it aliases ASIDs.**
`machine.h:653` — `hgatp_new(HGATP_MODE, asid, addr >> seL4_PageBits)` — passes the seL4
ASID straight into the 14-bit `hgatp.VMID` field. The branch *declares* the right design in
`include/arch/riscv/arch/64/mode/object/structures.bf:91-106` (`vmid_control_cap`,
`vmid_pool_cap`, `capVMIDBase`) and then **never references them from any C file** — a
tree-wide grep for `vmid_pool_cap|vmid_control_cap` outside the `.bf` returns nothing. The
compensation is `libsel4/sel4_arch_include/riscv64/sel4/sel4_arch/constants.h:50`, which
shrinks `seL4_NumASIDPoolsBits` from 7 to 5 under HE *"Assume 14-bit VMID for the RISCV64"*
— i.e. the ASID space is truncated to fit the VMID field. This is the gap the author names
in libvmm#246. It is the single largest piece of genuine kernel design work remaining and
it touches TLB-shootdown correctness.

**(c) The 16 KiB stage-2 root table is handled by a one-line global hack.**
RISC-V requires the G-stage root page table to be 16 KiB-aligned and 16 KiB in size.
`src/arch/riscv/object/objecttype.c:198-199`:

```c
    case seL4_RISCV_PageTableObject:
        return 14;          /* was: return seL4_PageBits; */
```

**Not** inside `#ifdef CONFIG_RISCV_HYPERVISOR_SUPPORT`. Every RISC-V page table — guest or
not, hypervisor build or not — becomes 16 KiB. The branch declares a proper
`s2_root_page_table_cap` in `structures.bf:52` and never uses it. The matching Microkit
hack is commit `103ed47 HACK: align allocation of page tables to 14-bits for RISC-V
hypervisor` and `tool/microkit/src/sel4.rs:217` (`Arch::Riscv64 => match config.hypervisor
{ true => Some(14), false => Some(12) }`) — note Microkit got the conditional right and the
kernel did not, so the two disagree in a non-hypervisor build. Fixing this properly means
introducing a real second page-table object type through retype, cap derivation, capDL,
Microkit's allocator and libsel4 — call it 2–3 weeks on its own.

**(d) Interrupt priority was reordered globally and left with dead code.**
`src/arch/riscv/machine/hardware.c:196-228`, unguarded: the source priority changed from
upstream's documented *external → software → timer* to *timer → software → external*, and
the old `else if (sip & BIT(SIP_STIP))` arm is still present and now unreachable, as is the
`#ifdef ENABLE_SMP_SUPPORT` `SIP_SSIP` arm. This is a behavioural change to the code path
that RV64 binary verification covers, made for the vtimer multiplexer, applied to all builds.

**(e) A kernel-safety check is commented out on the tip commit.**
`src/arch/riscv/c_traps.c:112-119`: the `CONFIG_DEBUG_BUILD` "KERNEL ABORT (exception within
s-mode)" check is `//`-commented, from commit `943e884c8 temp: comment out debug check`.
Still on the tip, four months later.

**(f) Known-broken instruction fetch.** `machine.h:620-632` ships `hlvxwu()` with the
comment *"This instruction on qemu returns incorrect instruciton. Do not use it for now"*,
encoded as a raw `.word 0x6832c373`. `vspace.c:424` instead reads `htinst`, with the
alternate path left behind `#if 0`. `htinst` is architecturally permitted to read zero for
many faults, so the VMM's "we gave you the faulting instruction so you need not walk the
guest page tables" contract is best-effort, not guaranteed.

**(g) Guest FPU is unresolved.** Commit `f3743d36f riscv-he: attempt to fix guest FPU
access` — "attempt" — and `include/machine/fpu.h` gains an unconditional-FPU-enable when a
TCB has a VCPU. This is one of the six files that conflicts on rebase (§2).

### 1.4 What is *not* there

- **No vPLIC and no AIA/IMSIC in the kernel.** Interrupt virtualisation is entirely
  userspace: `au-ts/libvmm@riscv` `src/arch/riscv/plic.c` (245 lines, with
  `#define PLIC_MAX_REGISTERED_IRQS 10` and a `// TODO: have no fucking idea where this
  number comes from`). Any hardware using AIA rather than PLIC needs new work in both repos.
- **No SBI forwarding in the kernel.** `RISCVEnvHypCall` is turned into a `seL4_Fault_VCPUFault`
  and handed to userspace; `libvmm/src/arch/riscv/sbi.c` (462 lines) emulates BASE, TIMER,
  IPI, RFENCE, HSM, SYSTEM_RESET, DEBUG_CONSOLE and the legacy console calls. That is the
  right architecture for seL4, and it is reasonably complete.
- **No `sel4test` coverage.** Upstream's only vCPU test, `apps/sel4test-tests/src/tests/vcpu.c`,
  is wholly inside `#ifdef CONFIG_ARM_HYPERVISOR_SUPPORT`. Upstream requires `sel4test`
  coverage for kernel features; writing it is unbudgeted work in every option below.

### 1.5 The other two repos

**Microkit (`Ivan-Velickovic/microkit@riscv_he`): 5 files, 97 insertions, 28 deletions, 4 commits.**
That is the whole thing. `build_sdk.py` +4 (turn `KernelRiscVHypervisorSupport` and
`KernelRiscVNumVTimers: 1` on for `qemu_virt_riscv64` and `hifive_p550`);
`libmicrokit/include/microkit.h` +28 (`microkit_vcpu_riscv_read_reg`/`write_reg`, and widening
the existing `#if defined(CONFIG_ARM_HYPERVISOR_SUPPORT)` guards);
`tool/microkit/src/sel4.rs` +27 (object sizes, `RISCVVCPUSetTCB` label, `RiscvVcpuSetTcb` args);
`tool/microkit/src/main.rs` +65/-28; `lib.rs` +1.

**libvmm (`au-ts/libvmm@riscv`): 1,822 insertions of new `src/arch/riscv/` + `include/libvmm/arch/riscv/` code**,
plus **373 insertions / 197 deletions** of arch-neutral refactoring (hoisting AArch64-shaped
interfaces in `src/fault.c`, `src/virq.c`, `src/linux.c`, `src/guest.c`, `src/dtb.c`,
`src/virtio/mmio.c`). The headline "20,794 insertions" is dominated by config blobs —
`examples/simple/board/qemu_virt_riscv64/linux_config` (7,161 lines),
`hifive_p550/linux.dts` (5,362), `buildroot_config` (4,785) and a GPL licence text (319).
Real code is ~2,200 lines. The agentOS-relevant virtio devices
(`src/virtio/{net,block,console}.c`) are arch-independent and unaffected.

**rust-sel4: not yet started, and not in anyone's plan.** See §2.3.

---

## 2. How stale is it, and what does rebasing cost?

### 2.1 Measured staleness

| Repo / branch | Tip | Tip date | Ahead | Behind (2026-10-06) |
| --- | --- | --- | ---: | ---: |
| `Ivan-Velickovic/seL4@microkit_riscv_he` | `6c4ffa2de` | 2025-07-21 | 29 (14 HE) | 307 |
| `Ivan-Velickovic/microkit@riscv_he` | `883a115a` | 2025-02-20 | 4 | 399 |
| `au-ts/libvmm@riscv` | `8b7011e9` | 2025-09-05 | 24 | 424 |
| `seL4/seL4@riscv_he_v0.6` *(dead end)* | `611ac4226` | 2022-03-22 | 33 | 874 |
| `seL4/seL4@riscv_hyp` *(dead end)* | `85c2f1d59` | 2019-10-30 | 13 | 1,720 |

The two official-repo branches are confirmed dead: draft H v0.6.1, `CONFIG_RISCV_HE` (a
different flag name from the live work), tracked by `seL4/seL4#812`, open since 2022-03-22
with **one comment** and last touched 2026-03-10. Do not build on them.

### 2.2 The rebase experiment — the single most decision-relevant number here

Extract the hypervisor-only delta (`git diff 3aafe9e0b 6c4ffa2de`, 2,340 patch lines) and
`git apply --3way` it onto today's `seL4/seL4` master:

```
exit=1
6 files applied "with conflicts"; 7 conflict hunks total
  CMakeLists.txt                                 1
  include/arch/riscv/arch/fastpath/fastpath.h    1
  include/machine/fpu.h                          1
  include/object/structures_64.bf                1
  src/arch/riscv/config.cmake                    1
  src/plat/qemu-riscv-virt/config.cmake          2
```

Everything else — **`vcpu.c`, `vcpu.h`, `machine.h`, `vspace.c`, `hardware.c`, `c_traps.c`,
`faults.c`, `objecttype.c`, all the `.bf` files, all of libsel4** — applied clean across
16 months and 307 upstream commits.

Three of the seven conflicts are cosmetic (CMake `-march` string rewrite; `config.cmake`
`CFILES` reindentation; `qemu-riscv-virt` memory default). Two are substantive and tell you
where the real risk lives:

- `include/machine/fpu.h` — upstream rewrote lazy-FPU switching (`switchLocalFpuOwner`),
  which is exactly the region the fork's unresolved guest-FPU fix touches.
- `include/arch/riscv/arch/fastpath/fastpath.h` — upstream added
  `benchmark_utilisation_switch` and `lazyFPURestore` where the fork inserts
  `vcpu_switch(thread->tcbArch.tcbVCPU)`. The fastpath is the hottest and most
  verification-sensitive code in the kernel.
- `include/object/structures_64.bf` — upstream changed the `UserException` fault block
  layout (`field code 28` + `padding 64` vs the fork's `field code 32` + `padding 60`),
  which interacts with `seL4/seL4#1327`, the open pre-RFC on giving fault message types
  unique IDs across configurations. The fork adds a new `VCPUFault` block to that same
  union; a fault-ID renumbering upstream would force a corresponding change here.

**Textual rebase is roughly a day.** What the experiment does *not* show is whether the
result still boots: upstream changed `src/kernel/boot.c` (12 commits, +127/-69),
`src/arch/riscv/kernel/vspace.c` (4, +13/-13), `src/arch/riscv/config.cmake` (5, +61/-72)
and `src/plat/qemu-riscv-virt/config.cmake` (2, +144/-179) in the same window. Budget **1 day
textual + 3–5 days semantic** per full-year rebase, and more when upstream touches FPU,
fastpath or fault encodings — which it did twice in this window.

### 2.3 Microkit: "re-done, not rebased" — true, and the reason matters more than the size

The maintainer's claim (au-ts/libvmm#246) is that the Microkit side "would need to be
re-done on the current Microkit, which uses capDL now." Checked:

- The fork's Microkit patch is 97 lines across 5 files. Trivially small.
- But upstream churn in exactly those files since the fork point is **not** small:
  `tool/microkit/src/main.rs` **38 commits, +38/-3,537** — the file was gutted;
  `tool/microkit/src/sel4.rs` 19 commits, +637/-1,229; `build_sdk.py` 65 commits, +528/-156;
  `libmicrokit/include/microkit.h` 10 commits, +338/-6.
- The cause is visible in the log: `4f62412 IOMMU: Integrate rust-sel4 capdl initialiser`,
  `4a9befa tool: update capdl object allocation algorithm`, `8277625 manual: memory region
  fixes for capdl`. `Cargo.toml:14-19` now pulls `sel4-capdl-initializer` and
  `sel4-capdl-initializer-types` from `github.com/seL4/rust-sel4`; `build_sdk.py:942` ships
  the built `initialiser` ELF in the SDK.

So "re-done, not rebased" is accurate — but the *re-doing* is small, because the concepts
transfer one-to-one (`ObjectType::Vcpu` sizes, one invocation label, one `microkit.h` API
pair). Call it **3–5 days**, not weeks.

**The part nobody has costed is the fourth repo.** Because object creation and TCB binding
now happen in the capDL initialiser rather than in Microkit's Rust tool, RISC-V VCPU support
has to exist in `seL4/rust-sel4`:

- `crates/sel4-capdl-initializer/src/initialize.rs:869-876` — the `vcpu_set_tcb` call is
  gated `#[sel4_cfg(any(all(ARCH_ARM, ARM_HYPERVISOR_SUPPORT), all(ARCH_X86_64, VTX)))]`;
  needs `all(ARCH_RISCV, RISCV_HYPERVISOR_SUPPORT)`.
- `crates/sel4/src/arch/riscv/object.rs` — `ObjectTypeRISCV` / `ObjectBlueprintRiscV` have
  `_4kPage, MegaPage, PageTable, GigaPage` and no `VCpu`.
- `crates/sel4/src/arch/riscv/fault.rs:22` — `pub enum Fault` has no `VCpuFault` variant
  (AArch64's does).
- `crates/sel4/src/arch/riscv/` has no `vcpu_reg.rs`; ARM has one per sub-architecture.
- Invocations are generated from libsel4's XML, so `object-api-arch.xml` propagates for free.

Estimate **300–500 lines, 1–2 weeks**, in a repo maintained by Colias Group with its own
review cadence — and one you would have to pin or fork as well, since Microkit pins it by
git revision.

### 2.4 Upstream churn rate, for planning

`seL4/seL4` master: 219 commits in 2024, 183 in 2025, 145 in 2026 YTD. Commits touching
`src/arch/riscv` + `include/arch/riscv` + the RISC-V libsel4 trees: **29 (2024), 27 (2025),
10 (2026 YTD)** — and trending down. Releases have accelerated: 13.0.0 (2024-07-01),
14.0.0 (2025-11-25), 15.0.0 (2026-03-31), 16.0.0 (2026-07-22) — three in the last eleven
months, i.e. **roughly quarterly**.

That is the maintenance clock. Not "how fast does RISC-V change" (slowly) but "how often
does this project have to re-qualify" (quarterly).

---

## 3. Maintenance burden after the fork

### 3.1 What forking does to `sdk-candidate`

The pipeline's whole value is in these lines:

- `.github/workflows/sdk-candidate.yml:42-47` — `git fetch --depth=1
  https://github.com/seL4/seL4.git e60776acc31097ca063806c257f07a3ec05eacf8` and the same for
  `seL4/microkit` at `ec86afdc`. **Pinned upstream commits, fetched from the canonical
  repositories by SHA.**
- `tools/sdk/candidate.mk:105` — `git apply tools/sdk/patches/sel4-e60776ac-cr2.patch`.
  **77 lines, 4 hunks, all x86 CR2 save/restore.** That is the entire delta this project
  currently carries against upstream seL4, and it is an obvious, self-contained bug fix.
- `tools/sdk/candidate.mk:85` — `sha256sum -c tools/sdk/cr2-kernels.sha256` over eight
  artefacts. **Byte-reproducible kernels.**

The good news is **structural: the pipeline already has exactly the right shape.** Carrying
the hypervisor delta does not require pointing at a fork repository. It requires replacing a
77-line patch with a ~2,340-line one, adding `qemu_virt_riscv64` to `--boards`
(`candidate.mk:119`), adding `gcc-13-riscv64-linux-gnu` to the workflow, and extending
`cr2-kernels.sha256`. Provenance stays "upstream commit + reviewed patch"; hash
verification keeps working unchanged.

The bad news is what that patch *means*. Today `tools/sdk/patches/` contains one artefact a
reviewer can read end-to-end in ten minutes and judge. After this, it contains a 2,340-line
kernel patch that introduces a new capability type, a new object type, a new fault type and
a new privilege mode — that nobody outside this project has reviewed, that no proof covers,
and that no `sel4test` exercises. The discipline survives mechanically and dies in substance.
If this is done, the patch must be split and reviewed as a series, and
`docs/TCB.md` must say in plain words that the riscv64 hypervisor kernel is a locally
maintained fork.

A second, separate patch is needed for Microkit (which `candidate.mk` currently patches not
at all), and a pin or fork of `seL4/rust-sel4`.

### 3.2 Realistic ongoing cost

Per upstream seL4 release (~4/year at the current cadence), for the kernel alone:

| Activity | Days |
| --- | ---: |
| Rebase the patch series, resolve conflicts | 1 |
| Semantic review of upstream changes in touched areas (fastpath, FPU, boot, fault encodings, RISC-V platform config) | 1–3 |
| Rebuild SDK, re-hash, update `cr2-kernels.sha256` | 0.5 |
| Boot + regression tests on QEMU riscv64, aarch64, x86_64 | 1 |
| Guest boot test (Linux under the VMM) on QEMU | 0.5 |
| Guest boot test on real hardware once it exists | 1–2 |
| **Per release** | **5–8** |

Plus, per year: Microkit re-sync (~3 days × 4 = 12), rust-sel4 pin bump and fix-ups (~1 day
× 4 = 4), libvmm rebase onto `main` (currently 424 commits behind; ~5 days first time, ~3
days/year after), and an irreducible incident budget.

**Steady state: 45–65 person-days/year ≈ 9–13 person-weeks ≈ 0.2–0.25 FTE.**

**First year: 60–90 person-days** on top of the initial engineering, because the first
rebase of libvmm (424 commits) and the first capDL-era Microkit implementation are one-offs.

Two things make that estimate optimistic and should be stated plainly:

1. **It assumes nothing breaks in a way that needs hypervisor expertise.** When a guest
   stops booting after a rebase, there is no upstream to ask, no `sel4test` to bisect
   against, and the debugging is in VS-mode CSR save/restore. Those bugs are measured in
   weeks, not days. One per year is a reasonable planning assumption.
2. **It assumes a bus factor above one.** Today the entire world's knowledge of this code is
   in two people's heads, neither of whom works here.

### 3.3 The asymmetry worth naming

Upstream seL4's RISC-V arch churn is ~10–30 commits/year and falling. The merge burden is
genuinely low. **The fork is not expensive to carry; it is expensive to be answerable for.**
Those are different costs and only the second one should drive the decision.

---

## 4. The verification question, stated precisely

This is the argument most likely to be made badly, and the framing this study started with
was itself wrong. Correcting it changes the answer.

### 4.1 What the proofs actually cover — verified against `seL4/seL4` master, not docs

seL4's verified configurations are literal files: `configs/*_verified.cmake` and
`configs/include/*_verified_include.cmake`. The proof applies to exactly the option
combination in those files and to nothing else. Reading them directly:

| | `KernelBinaryVerificationBuild` | Hypervisor | Notes |
| --- | --- | --- | --- |
| `ARM` (AArch32, EL1) | **ON** | off | |
| `ARM_HYP` (AArch32, EL2) | ON *(inherits `ARM_verified_include.cmake`)* | **on** | |
| `AARCH64` | *absent* | **`KernelArmHypervisorSupport ON`** | |
| `RISCV64` | **ON** | n/a | `KernelRiscvExtF OFF`, `KernelRiscvExtD OFF`, `KernelPTLevels 3`, `KernelFastpath ON`, `KernelMaxNumNodes 1`, `KernelRootCNodeSizeBits 19` |
| `RISCV64_MCS` | ON *(inherits)* | n/a | `KernelPlatform hifive`, `KernelIsMCS ON` |
| `X64` | *absent* | n/a | |

Only two architectures have binary verification: AArch32 and RISC-V 64. That is the
distinctive thing RV64 has.

### 4.2 The premise "the hypervisor config would be unverified, like AArch64 EL2" is false

`configs/include/AARCH64_verified_include.cmake:13` reads:

```cmake
set(KernelSel4Arch "aarch64" CACHE STRING "")
set(KernelArmHypervisorSupport ON CACHE BOOL "")
set(KernelVerificationBuild ON CACHE BOOL "")
```

There is **no** non-hypervisor AArch64 verified configuration. The AArch64 functional-correctness
proof *is* the EL2 hypervisor proof, and l4v carries the supporting theories —
`spec/abstract/AARCH64/VCPU_A.thy`, `spec/abstract/AARCH64/VCPUAcc_A.thy`,
`proof/invariant-abstract/AARCH64/ArchVCPU_AI.thy`, `spec/design/skel/AARCH64/VCPU_H.thy` —
and a real `handle_hypervisor_fault` for `ARMVCPUFault`, not a stub.

So the sentence "we already run guests on an unverified AArch64 hypervisor config, so this
is the same concession on a second architecture" is wrong **twice**: AArch64 EL2 is covered
by functional-correctness proofs, and RISC-V hypervisor mode is not analogous to it.

The correct analogy to RISC-V hypervisor mode is **x86 VT-x**: a virtualisation extension
with no abstract spec, no Haskell model, no refinement proof and no verified config at all.
agentOS already ships one of those (`x86_64_generic_vtx`). That is the precedent, and it is
a weaker one than the AArch64 framing suggested.

### 4.3 What l4v would need, concretely

For RISC-V, l4v has the shape of hypervisor support and none of the content.
`spec/abstract/RISCV64/Hypervisor_A.thy` is, in its entirety:

```isabelle
fun handle_hypervisor_fault :: "machine_word ⇒ hyp_fault_type ⇒ (unit, 'z::state_ext) s_monad"
  where
  "handle_hypervisor_fault thread RISCVNoHypFaults = return ()"
```

A repo-wide search for `*VCPU*` under any RISCV64 path returns nothing; AArch64 and ARM_HYP
each have four files. There is no RISC-V equivalent of `VCPU_A.thy` to extend — it would be
written from scratch, along with the Haskell model, the design spec, the refinement proof
and the invariant proof.

For costing: the seL4 team's own published figures (SOSP'09 and follow-ups) put a
cross-cutting kernel feature at **1.5–2 person-years to re-verify**, roughly 32% of the proof
effort originally invested in the affected subsystem. A new privilege mode with a new
capability type, a new object type and a new fault type is at the upper end of that. Treat
"and verify it" as **≥2 person-years of specialist Isabelle work that this project cannot
staff**, and remove it from the option set. It is not on the seL4 roadmap either —
`sel4.systems/roadmap.html` mentions RISC-V once, for MCS proof completion, and
virtualisation not at all.

### 4.4 The part that actually matters: the RV64 binary-verification result is not something this project has today

This is the finding that defuses the whole objection.

The verified `RISCV64` configuration is pinned to specific *platforms* —
`configs/RISCV64*_verified.cmake` name `hifive`, `hifive-p550`, `rocketchip`,
`rocketchip-zcu102`, `ariane`, `cheshire`, `polarfire`, `star64`, `bananapi-f3`.
**`qemu-riscv-virt` is not among them.** And beyond platform, the verified config differs
from any Microkit-built riscv64 kernel on at least four more axes:

| Option | Verified `RISCV64` | Microkit `qemu_virt_riscv64` (`build_sdk.py`) |
| --- | --- | --- |
| `KernelPlatform` | `hifive` (or 8 other named SoCs) | `qemu-riscv-virt` |
| `KernelRiscvExtF` / `ExtD` | **OFF** / **OFF** | not set upstream; **ON/ON** on the HE fork's boards |
| `KernelRootCNodeSizeBits` | 19 | **17** (`DEFAULT_KERNEL_OPTIONS`) |
| `KernelIsMCS` | off (or on, in `RISCV64_MCS_verified`) | **True** |
| `KernelMaxNumNodes` | 1 | `smp_cores=4` for this board |

So: **agentOS's riscv64 kernel is already outside every verified RISC-V configuration, and
would be even if the H-extension never existed.** The binary-verification result is a
property of a different build of the same source, on different hardware, with different
options. It is a reason to believe the *code* is good; it is not a certificate this project
holds.

### 4.5 So what is actually lost, precisely

Stated as narrowly as the evidence supports:

- **Lost in practice: nothing that is held today.** No agentOS riscv64 build is a verified
  configuration now.
- **Lost in principle: the option.** Today a riscv64 kernel could in future be brought *into*
  a verified configuration — move to a verified platform, turn off F/D, turn off MCS, match
  the CNode size — and inherit functional correctness *and* binary verification. Enabling
  `CONFIG_RISCV_HYPERVISOR_SUPPORT` closes that door permanently, because the proofs do not
  exist and will not be written on any timescale this project can plan against.
- **Lost in argument: the cleanest sentence in the project's story.** "riscv64 is the
  architecture whose kernel is covered by binary verification" is a defensible aspiration
  today and becomes false the moment the H-extension is on. If that sentence is load-bearing
  for the project's positioning, this is the real cost, and it is a positioning cost rather
  than an engineering one.
- **Additionally lost, and under-discussed:** the 167 unguarded lines in §1.3 mean the
  *non*-hypervisor RISC-V build also diverges from the verified source — `getActiveIRQ`
  reordering and the 16 KiB page-table change are in the verified build's code path. Taking
  the fork as-is contaminates the config that *could* have been brought into the verified
  set. That is a reversible engineering problem (it is the de-contamination work costed in
  option (b)) but it must actually be done, not assumed.

### 4.6 A hole that already exists on AArch64

None of seL4's verified configurations — AArch64 EL2 included — cover SMMU/IOMMU-mediated
device isolation. For a hypervisor running guests with passthrough devices, DMA isolation is
unverified on **every** architecture this project targets. Whatever is decided about RISC-V,
the guest-isolation story already has this hole in it on AArch64.

---

## 5. Options, costed

Effort is in person-weeks for one experienced kernel engineer who already knows seL4 and
RISC-V privileged architecture. Someone learning either on the job: multiply by 2–3.

### (a) Adopt the fork stack as-is, out-of-tree

Build the three (four) branches outside `sdk-candidate`, in a clearly-labelled experimental
target. Nothing enters the qualified pipeline; `docs/TCB.md` and the OS-claim gate do not
change.

| | |
| --- | --- |
| **Effort** | **3–6 weeks.** Toolchain + build (3–5 d; no riscv64 cross-GCC is installed locally, and the fork repos ship `flake.nix`). Microkit re-done on capDL (3–5 d). rust-sel4 VCPU support (5–10 d). libvmm rebase onto current `main`, 424 commits (5 d). agentOS integration of a real libvmm-backed VMM PD replacing the `__riscv` bare-metal jump in `platform/guest-vmm/guest_vmm.c` (5–10 d). |
| **Risk** | **Medium.** The individual deltas are small and rebase cleanly; the unknowns are rust-sel4 and whether the fork still works after a 307-commit rebase. Contained: a failed experiment costs the weeks and nothing else. |
| **Maintenance** | **~0** if frozen. Pin exact SHAs, never rebase, let it rot deliberately. If kept alive, it converges on (b). |
| **Buys** | A demonstrable riscv64 guest. Hardware bring-up experience ahead of silicon. A real answer to "could we." No claim of production readiness and no change to the project's verification story. |

### (b) Fork and maintain properly, rebasing onto upstream

Everything in (a), plus: clean up the unguarded changes so the non-hypervisor RISC-V build is
bit-identical to upstream; implement VMID allocation properly; introduce a real
`s2_root_page_table` object; restore the `c_traps.c` safety check; fix the `getActiveIRQ`
dead code; write `sel4test` coverage; carry it all through `sdk-candidate` as a reviewed
patch series.

| | |
| --- | --- |
| **Effort** | **16–26 weeks.** (a)'s 3–6, plus: de-contaminate the 167 unguarded lines (2 w); proper VMID pool + TLB shootdown (3–4 w); real stage-2 root PT object through retype/capDL/Microkit/libsel4 (2–3 w); resolve guest FPU (1–2 w); `htinst`/`hlvx` fault decode done correctly (1–2 w); `sel4test` RISC-V vCPU suite (2–3 w); SMP and multi-vCPU validation (2–3 w); `sdk-candidate` integration, patch-series split, hash discipline, docs (2 w). |
| **Risk** | **High**, and concentrated in the parts nobody has done: VMID/TLB correctness is the classic source of silent cross-guest memory disclosure, and you would be the only reviewer. |
| **Maintenance** | **45–65 person-days/year** steady state (§3.2), **60–90** in year one. Indefinite. |
| **Buys** | A riscv64 hypervisor the project can actually stand behind on its own hardware — and a permanent second kernel to qualify every quarter. |

### (c) Do the work and drive it upstream through the RFC process

| | |
| --- | --- |
| **Effort** | **(b)'s 16–26 weeks of engineering, plus 4–8 weeks of RFC authoring, review response and rework**, spread over the elapsed period. |
| **Elapsed time** | **12–30 months, with a material chance of never.** Measured from `seL4/rfcs`: RFC-18 (FPU context switching) 34 days open→approved but ~12 months approved→implemented; "Deprecate/remove old boards" 271 days; RFC-20 (Runtime Domain Schedules) 195 days. Against that: RFC-12, RFC-14 and RFC-16 have been open **>2.3 years** with no resolution, and **RFC-15 "Support CHERI/Morello in seL4"** — the closest analogue, a new-architecture-support RFC — has been open since 2024-06-14 with a body that is just a pointer to an old Jira issue. **RFC-22 "SBI capability"**, RISC-V-specific with an implementation PR already open (`seL4/seL4#1532`), has been open since 2025-10-24 — about a year — unresolved. No RFC comment period or SLA is published. |
| **Process requirements** | `docs.sel4.systems/projects/sel4/kernel-contribution.html`: discuss before filing; RFC detailing design decisions, performance impact and **verification implications**; `sel4test` coverage; either l4v proof updates or a demonstration the proofs are unaffected; for architecture work, prototype code is *required*, plus a stated plan for ongoing support and regression testing. Decided by the TSC, which can grant stage-1 approval, full approval, postpone, defer, require changes, or reject. |
| **Risk** | **Very high on schedule, low on technical outcome.** The telling fact: **Yanyan Shen and Ivan Velickovic — the two authors of all the RISC-V H-extension code that exists — are both on the TSC** (`sel4.systems/Foundation/TSC/`). The people who would approve this wrote it, and in four years no RFC has been filed. The blocker is not acceptance; it is that nobody has had the time. RISC-V virtualisation does not appear on `sel4.systems/roadmap.html` at all. |
| **Maintenance** | Eventually ~0 — upstream carries it. Until then, identical to (b). |
| **Buys** | The only outcome where this stops being a liability. Also buys goodwill and influence. Cannot be scheduled. |

### (d) Don't do it — stay native-PD-only on riscv64

Finish PR #298 (riscv64 builds, boots, loads a real PD set, proves a PD count in CI), add
`qemu_virt_riscv64` to `candidate.mk`'s `--boards`, and state plainly in `docs/TCB.md` that
riscv64 runs native PDs and does not run guests.

| | |
| --- | --- |
| **Effort** | **0 incremental** — PR #298 is already open (21 files, +1,847/-321, currently CONFLICTING against main). Adding riscv64 to the SDK build is ~1 day. |
| **Risk** | **Low.** The documentation risk is the real one: `system_desc_riscv64.c` declares 18 PDs including `guest_vmm_primary` and `guest_vmm_secondary`, and `platform/guest-vmm/guest_vmm.c`'s `__riscv` arm is a `jalr` into a bare-metal image with no vCPU and no stage-2 translation. If riscv64 is guest-free, those names must change. |
| **Maintenance** | Nil beyond the architecture itself. |
| **Buys** | An honest three-architecture claim: three architectures boot the platform, two run guests. It also preserves the *option* of one day bringing riscv64 into a verified configuration and inheriting both functional correctness and binary verification — see §4.4 for why that is an option this project does not currently exercise, and §4.5 for what enabling H actually forecloses. |

### Summary

| Option | Engineering | Maintenance/yr | Elapsed | Risk |
| --- | ---: | ---: | --- | --- |
| (a) Out-of-tree spike/prototype | 3–6 w | ~0 (frozen) | 1–2 months | Medium, contained |
| (b) Fork and maintain | 16–26 w | 45–65 d | 6–9 months | High, permanent |
| (c) Upstream via RFC | 20–34 w | ~0 eventually | 12–30 months, may never | Very high schedule risk |
| (d) Native-PD only | 0 | 0 | now | Low |

---

## 6. The cheap first step

**Yes — there is a two-week spike that is worth doing, and it should be the next action
regardless of which option is eventually chosen.**

### What it is

Out-of-tree, in a scratch directory, with a stated throwaway date. Nothing under
`tools/sdk/`, nothing in `.github/workflows/`, no change to `docs/TCB.md`, no entry in the
OS-claim gate.

| Day | Work |
| --- | --- |
| 1 | riscv64 cross toolchain. No `riscv64-*-gcc` is installed on this machine; the fork repos ship `flake.nix`, so `nix develop` is the shortest path. Confirm `qemu-system-riscv64` 11.1.1 reports `h` on `-cpu rv64` (ratified H v1.0 has been on by default on `virt` since QEMU 7.0). |
| 2–3 | Build `Ivan-Velickovic/seL4@microkit_riscv_he` + `Ivan-Velickovic/microkit@riscv_he` **at their pinned SHAs, unrebased**, via `build_sdk.py --boards qemu_virt_riscv64`. Resist the urge to rebase; the point is to reproduce the author's claim, not to improve it. |
| 4–6 | Build `au-ts/libvmm@riscv` `examples/simple` against that SDK, `MICROKIT_BOARD=qemu_virt_riscv64`, and boot the supplied buildroot Linux. **Success criterion: a shell prompt on the serial console.** |
| 7–8 | Second criterion: boot the `examples/virtio` system, which exercises `src/virtio/{net,block,console}.c` — the arch-independent devices agentOS actually depends on. |
| 9–10 | Write down what broke, how long each fix took, and what the fork's quality is like under the hood. Include a first-hand opinion on (b)'s 16–26 weeks. |

### Optional +1 week, and probably worth it

Repeat days 2–3 with the hypervisor delta rebased onto current `seL4/seL4` master using the
patch from §2.2 (7 conflicts, 6 files, all in config/headers). If a rebased kernel still
boots Linux, the central claim of §2 — "the recurring rebase cost is low" — stops being an
inference from conflict counts and becomes an observation.

### What it proves

- That the fork stack builds from source in 2026 with a modern toolchain.
- That Linux boots as a guest on riscv64 under seL4/Microkit **on QEMU**.
- First-hand code-quality evidence to calibrate the (b) estimate — the measurements in §1.3
  are static reading, and static reading systematically understates how long "attempt to fix
  guest FPU access" takes to finish.
- Whether the (optional) rebase holds.

### What it does not prove

- **Nothing about hardware.** The author's own status says serial IRQ passthrough is broken
  on HiFive P550. QEMU success says nothing about the silicon being built.
- **Nothing about correctness.** A Linux guest booting is a smoke test. It does not exercise
  VMID reuse, TLB shootdown across harts, guest FPU state across preemption, or any
  isolation property. The VMID aliasing in §1.3(b) would not show up.
- **Nothing about maintenance.** A single successful build says nothing about the fifth
  quarterly rebase.
- **Nothing about capDL.** The spike uses the *fork's* pre-capDL Microkit. The capDL
  re-implementation and the rust-sel4 work — the largest unknowns in option (a)'s estimate —
  are deliberately out of scope and remain unvalidated afterwards.
- **Nothing about agentOS.** No agentOS PD, root task or system descriptor is involved.

### What would make it a bad spike

Letting it become permanent. The failure mode is an experimental riscv64 hypervisor target
that works well enough to be demonstrated, never gets the de-contamination work in option
(b), and quietly becomes load-bearing. Set the throwaway date before starting it.

---

## Appendix A: measurement commands

Reproduce every number above:

```bash
git clone https://github.com/Ivan-Velickovic/seL4 sel4-fork && cd sel4-fork
git checkout microkit_riscv_he
git remote add up https://github.com/seL4/seL4 && git fetch up

# the hypervisor-only delta
git diff --stat 3aafe9e0b 6c4ffa2de           # 37 files, 1578+/25-
git log --oneline 3aafe9e0b..6c4ffa2de        # 14 commits

# the rebase experiment
git diff 3aafe9e0b 6c4ffa2de > /tmp/he.patch  # 2340 lines
git checkout --detach up/master
git apply --3way /tmp/he.patch                # 6 files with conflicts, 7 hunks

# upstream has nothing
git ls-tree up/master src/arch/riscv/object/  # interrupt.c objecttype.c tcb.c
git grep -n 'RiscVHypervisor\|RISCV_HYPERVISOR' up/master   # (empty)

# release cadence
for t in 13.0.0 14.0.0 15.0.0 16.0.0; do git log -1 --format="$t %ad" --date=short up-$t; done

# riscv churn per year
git log --format=%ad --date=format:%Y up/master -- src/arch/riscv include/arch/riscv \
  | sort | uniq -c
```

## Appendix B: primary sources

**Fork stack (cloned and measured 2026-10-06)**
- `https://github.com/Ivan-Velickovic/seL4/tree/microkit_riscv_he` — tip `6c4ffa2de`, 2025-07-21
- `https://github.com/Ivan-Velickovic/microkit/tree/riscv_he` — tip `883a115a`, 2025-02-20
- `https://github.com/au-ts/libvmm/tree/riscv` — tip `8b7011e9`, 2025-09-05
- `https://github.com/seL4/rust-sel4` — `sel4-capdl-initializer`, pinned by Microkit `Cargo.toml:14`

**Upstream seL4**
- `https://github.com/seL4/seL4` master `6df0b6ee6`; branches `riscv_he_v0.6` (874 behind), `riscv_hyp` (1,720 behind)
- `https://github.com/seL4/seL4/issues/812` — open since 2022-03-22, 1 comment, updated 2026-03-10
- `https://github.com/seL4/seL4/issues/1327` — open pre-RFC on unique fault-message type IDs
- `https://raw.githubusercontent.com/seL4/seL4/master/CONTRIBUTING.md` — "unverified platform" is acceptable upstream
- `https://docs.sel4.systems/Hardware/` — *"Support for the hypervisor extension is yet to be mainlined"*
- `https://sel4.systems/roadmap.html` — RISC-V appears only for MCS proof completion; no virtualisation item

**Process**
- `https://sel4.systems/Contribute/rfc-process.html`, `https://github.com/seL4/rfcs`, `https://sel4.github.io/rfcs/`
- `https://docs.sel4.systems/projects/sel4/kernel-contribution.html`
- `https://sel4.systems/Foundation/TSC/` — members include Yanyan Shen and Ivan Velickovic
- RFC timing data: `seL4/rfcs` PRs #12, #21, #24, #26, #30, #33, #35

**libvmm**
- `https://github.com/au-ts/libvmm/issues/246` — the maintainer's own status, 2026-05-26, open
- `https://github.com/au-ts/libvmm/pull/163` — closed unmerged 2026-05-26

**Verification (all read from fresh clones, 2026-10-06)**
- `seL4/seL4` `configs/include/AARCH64_verified_include.cmake` — `KernelArmHypervisorSupport ON`
- `seL4/seL4` `configs/include/RISCV64_verified_include.cmake` — `KernelBinaryVerificationBuild ON`, `KernelRiscvExtF/D OFF`, `KernelRootCNodeSizeBits 19`
- `seL4/seL4` `configs/RISCV64*_verified.cmake` — nine named platforms, none of them `qemu-riscv-virt`
- `seL4/l4v` `spec/abstract/RISCV64/Hypervisor_A.thy` — `handle_hypervisor_fault thread RISCVNoHypFaults = return ()`
- `seL4/l4v` `spec/abstract/AARCH64/{VCPU_A,VCPUAcc_A}.thy`, `proof/invariant-abstract/AARCH64/ArchVCPU_AI.thy`
- `https://docs.sel4.systems/projects/sel4/verified-configurations.html`
- `seL4/microkit` `build_sdk.py:46-66` (`KernelIsMCS: True`, `KernelRootCNodeSizeBits: "17"`), `:327-336` (`qemu_virt_riscv64`)
- Verification cost reference: Klein et al., *seL4: Formal Verification of an OS Kernel*, SOSP'09 — ~11 person-years proof effort; cross-cutting feature re-verification 1.5–2 person-years (~32% of prior investment)

**Local**
- `/Users/jkh/Src/agentos/tools/sdk/candidate.mk`
- `/Users/jkh/Src/agentos/tools/sdk/patches/sel4-e60776ac-cr2.patch` (77 lines)
- `/Users/jkh/Src/agentos/tools/sdk/cr2-kernels.sha256`
- `/Users/jkh/Src/agentos/.github/workflows/sdk-candidate.yml`
- `/Users/jkh/Src/agentos/kernel/agentos-root-task/src/system_desc_riscv64.c`
- `/Users/jkh/Src/agentos/platform/guest-vmm/guest_vmm.c`
- `/Users/jkh/Src/agentos/.sdd/riscv-guest-feasibility.md` — the prior research note this study builds on
- PR #298 `riscv/arch-parity` — 21 files, +1,847/-321, open, CONFLICTING
