# Changelog

All notable changes to agentOS are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Fixed

- `fault_handler` was dead on arrival on every architecture. Its
  `fault_ring_vaddr` global was declared in `.bss` and assigned nowhere in the
  tree — no linker script, no `--defsym`, no generated header, no root-task
  provisioning — so `fault_handler_init()` stored the ring header through a
  NULL pointer and the PD died at its first store after entry, before it could
  log anything. On AArch64 and x86_64 this was additionally invisible because
  `serial_pd` owns the UART by then, so the kernel's fault report never reached
  the console; it surfaced only during riscv64 bring-up. The root task now
  allocates one 2 MiB frame and maps it into the PD's VSpace at
  `AOS_FAULT_RING_VA` (`platform/include/platform/fault_ring.h`) before the
  thread starts, failing the boot closed if that mapping fails, and records the
  grant in the capability ledger so the published authority page stays honest.

### Added

- **riscv64 architecture parity.** riscv64 went from "does not compile" to
  booting the platform under QEMU with a real protection-domain set:
  - a riscv64 arm of `kernel/loader/` and `src/start_riscv64.S`, so QEMU boots
    a loader ELF that places seL4 and the root task and jumps to the kernel,
    instead of handing the raw `"AGENTOS\0"` container to `-kernel` and
    watching OpenSBI print its banner into silence;
  - PDs loaded from the same signed, embedded bundle AArch64 and x86_64 use.
    `main.c`'s claim that riscv64 loaded PDs "via the seL4 extra BootInfo
    path" named a consumer with no producer anywhere in the tree, so no PD had
    ever started there. `AGENTOS_HAS_PD_BUNDLE` is now 1 on all three
    architectures and there is no bundle-less target left;
  - a descriptor matching the AArch64 topology — driver PDs own a device
    class, virtualizers mux it — in place of the pre-virtualizer HURD-era PD
    list;
  - `make test-riscv64`, which asserts the **exact** PD count, manifest
    signature verification, the exact root-task fault count and
    `[rt] boot complete`. A boot marker alone proves nothing, as
    `x86_64_generic` shows by reaching one with an entirely empty descriptor.
- **riscv64 is built by CI.** A new `riscv64-root-task-build` job builds the
  pinned seL4 riscv64 board from the same commits and patch the SDK pipeline
  uses, then compiles every riscv64 PD and links `root_task.elf`, asserting the
  result is an EXEC RISC-V ELF defining `_start` and `root_task_main` and
  carrying the embedded PD bundle. riscv64 was broken by a single duplicate
  `case` label — correct on AArch64, a hard error on RISC-V — for an unknown
  length of time, because nothing in CI ever compiled it.
- `qemu_virt_riscv64` in `tools/sdk/candidate.mk`'s `--boards`, with a riscv64
  cross-GCC and `qemu-system-riscv64` in the `sdk-candidate` workflow, and its
  kernel hash in `tools/sdk/cr2-kernels.sha256`
  (`8445adeb…`, measured from a real pipeline run and reproduced by the next
  one). Same pinned commits, same single patch, no forked seL4 or Microkit.
  RISC-V uses build_sdk.py's default bare-metal `riscv64-unknown-elf` triple
  rather than a `riscv64-linux-gnu` override, because Ubuntu's `linux-gnu` GCC
  defaults to PIE and Microkit's own loader `crt0.S` then fails to link with
  `dangerous relocation: The addend isn't allowed for R_RISCV_GOT_HI20`.
- `make test-fault-handler` (in `gate:` and as an explicit `os-claim-gate` CI
  step, since no CI job invokes `make gate`). `fault_handler` has no service
  endpoint and no device, so nothing else in the suite moved when it failed to
  start. The PD now runs a bounded self-check at boot — it round-trips writes
  at ring offset 0, at the last entry slot, and at the final byte of the
  256 KiB ring, then re-verifies the header — and the harness matches the
  emitted line byte for byte, including the mapped VA and the ring span, so a
  ring mapped short or at the wrong address fails too. This proves liveness and
  usable storage; it does **not** prove fault delivery, since no PD is made to
  fault and no entry reaches the ring through a real seL4 fault IPC.
- `make test-authority` now asserts `fault_handler`'s published row: exactly
  two frame capabilities (its IPC buffer plus the one fault-ring frame) and no
  IRQ handler.

### Changed

- `sdk-candidate-check` now refuses an SDK that contains a kernel with no
  recorded hash in `tools/sdk/cr2-kernels.sha256`, printing the computed
  hashes when one is missing. `sha256sum -c` only checks the lines it is
  given, so a board added to the build list but not to the manifest would
  otherwise pass with its kernel entirely unverified. The requirement is
  scoped to boards actually present in the SDK under test, because this target
  is a prerequisite of every ordinary `make build` and therefore also runs
  against published artifacts that predate a newly added board.
- The `hurd-services-build` CI matrix lost its `qemu_virt_riscv64` leg. It
  never referenced `matrix.board`: both legs ran `clang -fsyntax-only` against
  host headers and `make -n ... ARCH=aarch64`, so it displayed a check named
  for riscv64 while running aarch64 work twice. It was the only `riscv` string
  under `.github/`.
- The T6 child-spawn fault probe asserts the **provenance** of the fault IPC,
  not only its shape. Root no longer mints a badged copy of its own fault
  endpoint into `child_spawn_parent`'s CNode; it keeps that capability in its
  own CSpace and installs it on the child's TCB itself, over a separate
  single-shot installer endpoint the parent calls with the child's TCB
  capability. No protection domain holds a capability to root's fault endpoint
  or any derivative of it, so `AOS_CHILD_SPAWN_PROBE_BADGE` can only reach
  root's fault loop in a kernel-generated fault IPC.

  Stripping send rights from the parent's copy was tried first and does not
  work: seL4's MCS `validFaultHandler()` requires a fault-handler capability to
  carry Send plus Grant or GrantReply, and `seL4_TCB_SetSchedParams` refuses a
  rights-stripped copy with `seL4_InvalidCapability`.

- `aos_child_spawn()` gained `fault_install_ep` / `fault_install_label` for
  this delegated-install shape. When they are used the SchedContext is bound
  with `seL4_SchedContext_Bind` rather than a second `seL4_TCB_SetSchedParams`,
  because the latter unconditionally rewrites the fault handler.

- The T5 capability-lending probe needed no change: root already minted the
  borrower's badged fault endpoint into its own CNode and installed it on the
  borrower's TCB. Comments claiming T5 shared T6's forgeability gap were
  wrong and have been corrected.

### Known limitations

- **riscv64 runs no guest operating system, permanently, and nothing in this
  repository changes that.** Upstream seL4 has no RISC-V hypervisor extension
  in any release 13.0.0–16.0.0 or on master; the working code lives in a fork
  stack whose own maintainer names seL4 as the blocker, and enabling the
  H-extension would forfeit the RV64 binary-verification result that is
  riscv64's distinguishing property in this lineup. See
  `docs/superpowers/specs/2026-10-06-riscv-hypervisor-fork-feasibility.md`.
  `guest_vmm.c`'s `__riscv` arm is a same-privilege `jalr` with
  `_guest_kernel_image` permanently NULL and is not guest support.
- **`make test-riscv64` run from a developer's `make sdk` still skips.** In CI
  it is a real gate: `os-claim-gate` installs the artifact the same workflow
  run built, which now carries `qemu_virt_riscv64`, and the step reports
  `9 of 9 PDs started, [rt] boot complete, 0 known fault report(s)`. But
  `make sdk` fetches the *published* release asset, which predates the board,
  so the target's loud-skip guard fires locally until that asset is
  republished. Use `make test-riscv64 SEL4_SDK_VERSION=2.1.0` meanwhile.
- **The packaged SDK archive no longer matches `SDK_CANDIDATE_ARCHIVE_SHA256`,
  and will not until it is republished.** That pin names the release asset
  `make sdk` downloads; adding a board necessarily changes what the pipeline
  packages. The pin is deliberately left alone, because raising it ahead of
  publication would break `make sdk` for every developer. `sdk-candidate`
  reports the divergence every run with both digests and the two-step fix
  (publish the archive, *then* set the pin). The same republication is what
  makes a developer's `make sdk` carry the riscv64 board.
- **The T10 trust-anchor tier model is proven on one architecture, not
  three.** All five `make test-trust-anchor` probes run on
  `qemu_virt_aarch64`. riscv64 now compiles in a real tier, key and gating
  decision with no automated tier probe of its own; `make test-riscv64` covers
  the signature-verified positive path only.
- The child-spawn probe's residual: root cannot tell one of the parent's
  threads from another, so the parent chooses which TCB it presents to the
  installer. The marker proves that a thread the parent created really took an
  unmapped read fault at the withheld address, as reported by the kernel; it
  does not by itself prove that thread was the child.

## [0.6.0] - 2026-10-04

Trust and delegation baseline: the corrective actions from the 2026-10-03
architecture audit. Capabilities now have a lending primitive, a hierarchical
delegation path, and an image-verification trust model that works on hardware
with no key store.

### Added

- Hierarchical delegation (T6). A protection domain may create a child domain
  at run time and endow it from its own authority: the parent retypes the
  child's CNode, VSpace and TCB from an untyped pool root granted it at boot,
  mints rights-reduced derivatives of capabilities it already holds, and starts
  the child only after every endowment succeeded. `make test-child-spawn`
  proves on target that the child reads an exact byte pattern from an endowed
  frame (cross-checked by physical address against the parent's own
  capability), faults on authority the parent withheld (asserted on exact
  badge, address, direction and fault type), does not start at all when an
  endowment fails, and is reported in the parent's endowment ledger with
  matching counts.

  Endowment is deliberately **not** capability lending. Lending revokes the
  lender's own original and so destroys every derivative system-wide, which is
  correct for a loan and catastrophic in spawn teardown; a failed spawn of one
  child would have stripped a running sibling of its authority. Endowment is a
  lifetime grant: it mints directly, and teardown deletes the child's CNode.

- Trust anchor tiers (T10), extending protection-domain image verification from
  a single compiled-in vendor key to four tiers on the Linux shim/MOK model:
  `AOS_ANCHOR_NONE` (development — verifies and reports, does not gate),
  `AOS_ANCHOR_VENDOR` (the previous behaviour), `AOS_ANCHOR_MOK` (a
  machine-owner key), and `AOS_ANCHOR_HARDWARE` (a defined key source that is
  **not implemented** and reports unavailable). The tier is compiled into the
  image, announced at boot, and exposed through inspect.

  Development mode does not skip verification — it verifies and reports without
  gating. `boot_verify_pd_digest()` takes no tier argument and has no tier
  branch: digests are always computed and compared, and the tier is consulted
  only afterwards. This is mutation-tested — short-circuiting the comparison
  makes the proof fail in exactly the shape of the anti-pattern it guards
  against. An ordinary `make build` produces a gating vendor image, and that is
  asserted by `make test-inspect`, not merely argued.

  Anchor selection is now a tracked build input. It previously was not: what
  regenerated the bundle on an anchor change was an unrelated relink winning an
  mtime race, which could leave a non-gating image on disk while every
  operator-visible signal said vendor.

### Fixed

- Collapse a duplicated `gate:` rule into one prerequisite list. GNU make
  unions prerequisites across rules, so no proof was being skipped, but editing
  one of the two lines would have silently removed a proof from the release
  gate with no error.
- Remove a contradictory `Qualification boundary` paragraph in `docs/TCB.md`
  that asserted both that CI's `os-claim-gate` result is the qualifying
  evidence under the pinned SDK and that the results were not qualified under
  it.

### Limits

Stated here because the project treats overclaiming as the primary defect:

- The machine-owner anchor **stands alone**: it requires an owner key and
  treats the vendor key as optional. On a MOK-only machine, vendor-signed
  images — including agentOS's own release artifacts — do not verify until the
  owner signs or counter-signs them.
- MOK does **not** defend against the machine owner, and no anchor available
  today does: an owner with physical access can replace the boot chain. MOK
  enrolment is build-time-provisioned; there is no runtime physical-presence
  enrolment flow.
- `AOS_ANCHOR_HARDWARE` claims nothing about TPM or measured boot. There is no
  attestation and this is not a measured-boot chain.
- The endowment ledger is **PD-local** and reports rather than proves. seL4
  exposes no capability-enumeration syscall, so nothing here verifies the
  subsetting invariant — the kernel enforces that independently. A
  runtime-created child does not appear in `agentctl authority` output.
- The fault-probe oracle asserts the shape of a fault IPC, not its provenance.
  (Fixed for T6 after this release; see Unreleased. T5 was already sound.)
- A digest mismatch under the machine-owner anchor is not target-proven, and
  the gating-tier-with-no-key probe asserts an absence bounded by a
  loader-stage marker rather than a positive refusal marker.
- `riscv64` architecture parity is **not** in this release; it is open as a
  pull request.

## [0.5.1] - 2026-09-27

### Documentation

- Complete the v0.5.0 native evidence index with the retained Arch
  graphics/input regression history and final Sway/WayVNC desktop receipt.
  Correct the earlier integration note's pending desktop status. These are
  historical qualification records, not new runtime fixes or 0.5.1 test results.

## [0.5.0] - 2026-09-26

### Added

- Add a checksum-pinned vanilla Arch Linux x86_64 installation profile that
  installs to persistent virtio block storage, reboots through the canonical
  agentOS console, network and block backends, and proves key-only SSH plus
  terminal, identity, process, network, DNS and package-lifecycle operations.
- Add an x86 desktop profile over the canonical virtio GPU and input
  virtualizers. Native Intel qualification covers DRM/KMS enumeration, exact
  keyboard and pointer delivery, an active Sway output with both agentOS input
  devices, and a non-empty 1024x768 WayVNC RFB capture.
- Add reproducible desktop package provisioning with exact archive database,
  package and detached-signature hashes followed by normal pacman signature
  verification. No signature bypass is used.

### Changed

- Consolidate generated objects, archives, executables, images, Cargo output
  and test evidence under `_build`, with a standalone cleanup path.
- Wait for a newly committed framebuffer sequence before desktop capture, so
  asynchronous display readiness cannot accept the initial black frame.
- Keep `agentos_gui` as the AgentOS control plane and RemoteOS-SDL as the
  shared graphical presentation and input backend. The architecture decision
  remains a design record; no human UI is embedded in this repository.

### Qualification boundaries

- Desktop and persistent-install results are from one-vCPU Intel KVM with
  nested VMX and a pinned external kernel/initramfs. They do not qualify the
  installed bootloader, kernel updates, Omarchy, arbitrary physical hardware,
  or host display/input passthrough.
- The retained native desktop archive has SHA-256
  `1bf2287b690a4b92064f5654748adc3b51d2258621d5feb256085f5acd007155`.
  The working disk was derived by offline journal recovery after interrupted
  diagnostics; the original base and failed images remain preserved.

## [0.4.1] - 2026-09-25

### Added

- Add a transport-independent RemoteOS protocol-v2 client foundation with
  bounded framing, parsing and binary pixel upload, plus deterministic mock
  coverage and live headless RemoteOS-SDL interoperability. This is host-side
  protocol evidence only: the release image does not yet contain a display
  relay, and graphical CC framebuffer/input integration remains unqualified.
- Record the GUI architecture decision: `agentos_gui` remains the AgentOS
  control plane while RemoteOS-SDL is the common graphical presentation/input
  backend. The future relay stays outside the TCB and uses existing CC,
  framebuffer-observer and input-virtualizer contracts.

## [0.4.0] - 2026-09-22

### Added

- A VMX-backed x86 guest composition with pinned UEFI firmware, generated ACPI,
  bounded guest memory, and agentOS-emulated network, block and console devices.
  Retained Intel qualification covers Debian userspace, managed recreation,
  two-vCPU workloads, writable-media isolation and detached snapshots. These
  results include a final-SDK 2 GiB terminal teardown and zeroed-pool reuse
  check. Exact revisions and limits are retained in
  `docs/evidence/2026-09-22-release/resource-profiles.json`.
- Live AArch64 framebuffer and input services, emulated virtio-gpu and
  virtio-input, bounded CC frame export, and keyboard/pointer delivery. The
  external `agentos_gui` consumes these contracts; no human UI is embedded in
  the OS repository. GUI revision `41b6c1e` passed native binary-IPC rendering,
  exact keyboard/pointer delivery, held-input release after abrupt disconnect,
  reconnect, suspend/resume and normal shutdown against core `c2a25050`.
  See `docs/evidence/2026-09-22-release/native-gui.json`.
- Managed AArch64 Debian reconstruction after teardown, including fresh queue,
  input and graphics bindings. Retained qualification checks a second guest
  identity, stale-handle rejection, pinned SSH and a persistent disk witness.

### Changed

- Use pinned Debian stable alongside FreeBSD for the default dual-guest
  scenario. Dated images and checksum manifests, key-only provisioning and
  extracted boot artifacts replace moving integration media. Focused legacy
  Ubuntu gates remain available.
- Select the approved Microkit 2.3.1 / seL4 e60776ac SDK with the scoped CR2
  preservation patch through a shared default-version file. The reproducible
  target bundle includes kernel wrappers, headers, linker scripts and licenses;
  distribution artifacts retain the source, patch and build recipe. The
  prerelease source-build path is documented in `docs/x86-cr2-candidate.md`;
  the default download requires publication of the v0.4.0 release asset.

- Route dynamic guest control from CC-PD directly to `vm_manager` and remove
  `vibe_engine` from the boot image. Public handles remain distinct from backend
  slots, backend failures propagate, and failed-start rollback retains a
  recoverable handle when cleanup fails.
- Route guest consoles through a separate `serial_virt` PD with isolated
  VMM/frontend pages, authenticated attachment and bounded byte queues.
  Retain descriptor progress during backpressure. Qualify sustained output
  and concurrent Ubuntu/FreeBSD SSH, including FreeBSD suspend/resume,
  peer survival during Ubuntu destruction, and authenticated SSH after fresh
  Ubuntu creation. Local and hosted recreation gates passed at `c2a25050`;
  `docs/evidence/2026-09-22-release/dual-recreation.json` retains both results.

### Qualification boundaries

- Intel results use Linux KVM with nested VMX; graphics/input results use
  AArch64 QEMU and a Linux/X11 native GUI. These do not qualify arbitrary
  physical hardware, x86 desktop graphics, Omarchy, or live memory snapshots.
- SDK installation verifies the pinned target archive. The coordinated GUI
  remains an external source build; use the recorded revision above. Disk
  snapshots are detached, offline snapshots. Back up persistent guest media
  before migration; no cross-version live-state restore is promised.

### Fixed

- Map each VMM's network queues in its own large page, and keep the driver's
  transfer page out of VMM address spaces. net_virt alone maps both tiers.

- Bind virtualizer ATTACH client, VMM slot, and block-media selection to
  root-minted capability badges. Reject spoofed assignments before state or
  driver operations. Net attach is contract v2; block attach is contract v3.

- Give each block virtualizer client its own mapped large page. VMMs no
  longer map the other client's queues and payloads or the RAM-disk page.
  Bump the block attach contract to version 2 for the changed queue layout.

- Honor the guest's virtual-timer interrupt mask during AArch64 WFI recovery;
  an expired masked timer no longer causes a synthetic interrupt. Preserve
  pending/inflight IRQ guards and rearm the independent seL4 VPPI when the
  timer output is deasserted.

- Require the guest-I/O summary in protected-main release validation, alongside
  the existing boot and host checks. Boot-only branch protection no longer
  satisfies publication policy.

- Correct `log_drain` serial OPEN and WRITE payloads so its output reaches
  `serial_pd` on release kernels. A host round-trip test checks UART bytes,
  nonzero slot isolation, chunked writes, and ring draining; AArch64 QEMU
  tests now require the driver's log-drain readiness output.

## [0.3.0] - 2026-09-11

### Changed

- `make gate` is now an honest OS-claim gate: it runs the host suite, the
  aarch64 and x86_64 `GUEST_OS=none` boot tests, and `gate-guest-io` (the
  Buildroot virtio-net and virtio-blk proofs and the Ubuntu virtio-console
  proof). CI gains an `OS-claim gate (boot + guest net/blk/console proofs)`
  summary job that passes only when all of those pass. `GUEST_OS=none` alone
  is documented as a stub VMM that proves PD load only.
- The two-hour Ubuntu Casper live-media proof moved out of per-push CI into a
  scheduled `ubuntu-live-nightly.yml` workflow. It had never passed on `main`
  since it was added and turned every push red.
- The default aarch64 image boots 13 PDs (nameserver, log_drain, serial_pd,
  vibe_engine, virtio_blk, block_pd, net_pd, net_virt, blk_virt,
  guest_vmm_primary, vm_manager, cc_pd, fault_handler). The v0.2.2 manifest
  bundled 39 ELFs, 20 of which the root task never started; the descriptor
  also dropped controller, event_bus, init_agent, agentfs, vfs_server,
  net_server, framebuffer_pd, and usb_pd. `cc_pd` now prints the
  `agentOS boot complete` marker.
- `docs/TCB.md` describes what boots today separately from the target shape,
  names `cc_pd` as the console driver, and states per invariant whether it is
  held. Invariant 2 (the virtualizer is the only mux) is held for net and
  block and not yet for console.
- Host tests under `tests/platform` no longer assert by grepping source text.
  Of 95 assertions, 4 behavioral tests remain, 26 architecture invariants moved
  to `tests/platform/lint_source_invariants.c` (run by `make lint-source`
  inside `make test-host`), and 65 implementation-pinning checks were deleted.

### Removed

- About 15,400 lines of code compiled by nothing: `libs/libvmm` (duplicate),
  `libs/libraft`, `libs/libagent`, six unreferenced `services/` trees, five
  `userspace/` trees, five `agents/` trees, twelve orphan root-task sources,
  the root `CMakeLists.txt`, and `sdk/python`. `services/msgbus` and
  `services/capstore` were among them; their endpoint-pool and cascading
  revocation logic is recoverable from commit `08ae7f37`.
- The vestigial Python setup in the `validate-topology` CI job.

### Fixed

- `xtask qemu-test` honors `AGENTOS_TMP_DIR` for QEMU logs and control
  sockets, so guest proofs run from a git worktree on macOS (104-byte Unix
  socket path limit).
- The qcow2 conversion unit test skips instead of failing when `qemu-img` is
  not installed.

### Added

- `net_virt` is a real protection domain: the network virtualizer owns no
  device frame or IRQ, is the only PD speaking `net_pd`'s raw-frame contract,
  and moves frames through the sDDF queues in the shared net frame with
  NBSend kicks and `RX_READY` notifications. The VMM does one ATTACH call and
  never carries a frame over IPC. TCB.md invariant 2 now holds for networking
  (contract: `include/contracts/net_virt_contract.h`, version 1).
- `blk_virt` is a real protection domain: the block virtualizer owns no
  device frame or IRQ, is the only PD holding the `virtio_blk` endpoint or
  (besides the driver) mapping its bounded DMA window, and moves requests
  through the sDDF queues in a root-provisioned 4 MB shared block region with
  NBSend kicks and `RESP_READY` notifications. The VMM does one ATTACH call
  and never carries a block request over IPC; two guests can no longer race
  in the driver's DMA window. TCB.md invariant 2 now holds for block
  (contract: `include/contracts/blk_virt_contract.h`, version 1). The shared
  block region moved from `0x20200000`, which collided with the secondary VMM
  image reservation, to `0x28000000`. `blk_virt` reads the media write
  policy (`AOS_HOST_BLK_INFO_READ_ONLY`) from the driver INFO reply at ATTACH
  and publishes it in the client storage_info page, so writable media and
  `VIRTIO_BLK_F_FLUSH` (#117) reach the guest through the virtualizer.

### Documentation

- `README.md` rewritten for the post-audit platform (what is proven today vs.
  target, the booted PD set, quick start, current tree, proof levels).
- New `docs/QUICKSTART.md` (clone to boot, guest proofs, demo, logs,
  troubleshooting), `docs/DEVELOPER_GUIDE.md` (boot flow, declaring a PD,
  contracts, notifications, adding a virtualizer or driver PD using `net_virt`
  as the template, guest profiles, testing policy), and `docs/README.md`
  (index marking current vs. historical documents). `DESIGN.md` carries a
  banner pointing at `docs/TCB.md` and the README as current truth.

### Known limitations

- `serial_virt` is not yet a separate PD; console bytes still reach `cc_pd`
  by IPC from the VMM.
- `log_drain`'s `MSG_SERIAL_WRITE` request layout does not match `serial_pd`'s
  handler, so generic PD log output is silent on the release kernel; PDs that
  must emit markers use the serial transfer page directly.
- `vibe_engine` remains in the image because `cc_pd` relays dynamic-guest
  creation through it to `vm_manager`.
- The `main` branch protection still names the boot-only gate job as its
  required check; switching it to the OS-claim summary job is an admin action.

## [0.2.2] - 2026-09-09

### Changed

- Advance the workspace release after reconciling the duplicate GitHub issue
  backlog into the canonical MAC task ledger and confirming that no stale
  branches or worktrees remain.

### Notes

- This is an administrative maintenance checkpoint. Target and runtime code
  are unchanged from v0.2.1, and the release retains the same OS claim.

## [0.2.1] - 2026-09-09

### Added

- Versioned TOML guest-personality profiles with bounded validation and a Rust
  compiler that emits compact target runtime manifests.
- Data-driven host recipes for artifact acquisition, provisioning, console
  matching, and guest tests, including planned Debian, Arch, and Omarchy
  profiles alongside the qualified Buildroot and Ubuntu paths.

### Changed

- Select guest boot, memory placement, devices, lifecycle, and tests from
  capabilities in compiled manifests instead of distribution-specific target
  branches.
- Share one guest-neutral VMM implementation across Linux and FreeBSD
  personalities while retaining generic boot-protocol executors.

### Known limitations

- Debian, Arch, Omarchy, and the Ubuntu live-media login path are roadmap
  profiles and are not qualified by the v0.2.1 OS release claim.

## [0.2.0] - 2026-09-09

### Security

- Give each physical device frame and IRQ one agentOS driver PD owner; Linux
  and FreeBSD guests receive no host MMIO or interrupt capabilities.
- Translate and bounds-check guest physical addresses for VirtIO net, block,
  console, sound, and diagnostic paths instead of identity-mapping guest RAM
  into a VMM.
- Isolate Linux and FreeBSD guest VSpaces and reserve their RAM before loading
  ordinary PD images.
- Preserve queue ownership under backpressure: receive traffic drains across
  descriptor cycles, and rejected console frames are retried without silently
  diverting bytes to an inactive early console.
- Require distinct, ephemeral, key-only SSH identities for automated guest
  access. The experimental desktop protocol is designed to remain confined to
  an authenticated SSH tunnel.
- Enforce the repository-wide C, Rust, and Assembly language policy and remove
  the repository-owned interactive UI path.

### Added

- VMM-emulated VirtIO net, block, and console devices backed by agentOS-owned
  driver and virtualizer queues. QEMU VirtIO transports are host-hardware
  stand-ins only and are not advertised to guests.
- Guest-visible packet, block-I/O, and bidirectional console proofs for the
  Buildroot and Ubuntu AArch64 paths.
- Ubuntu 26.04 and FreeBSD 15.0 AArch64 guest lifecycle paths with isolated
  memory, separate network identities, independent block media, and a staged
  concurrent authenticated-SSH acceptance gate.
- An experimental Ubuntu software-desktop gate that starts the graphical
  workload in the guest and can record a bounded RFB 3.8 raw-frame checksum
  through SSH. It is not part of the v0.2 OS release claim.
- A native agent sDDF network client alongside the VMM clients, demonstrating
  that native components and compatibility guests share the same virtualizer
  boundary.
- An evidence-bound Rust release workflow with read-only planning, metadata
  preparation, exact-revision gates, authorized publication, annotated tags,
  receipts, and remote verification.
- A repository-owned Rust PDF renderer for the release systems/security deck,
  with source- and fact-ledger hashes plus structural QA evidence.
- Executable GitHub Actions coverage for host contracts, both root-task
  architectures, Buildroot guest I/O, and the Ubuntu agentOS-VirtIO path.

### Changed

- Make is the canonical setup, build, test, demo, and release interface.
- Host setup fails closed when required guest-staging or target tools are
  unavailable and uses one configurable external Microkit SDK.
- Guest lifecycle and console handling share common flavor-neutral validation,
  while Linux and FreeBSD retain separate VMM implementations for now.
- Release claims are explicitly tiered: host tests, target tests, guest-visible
  behavior, concurrent authenticated access, and desktop evidence are distinct
  gates.

### Known limitations

- The agentOS target and emulated-device paths are qualified on QEMU AArch64.
  The x86_64 target proves the reduced agentOS root-task topology only; it does
  not yet execute an x86 guest.
- Ubuntu live-media boot is not release-qualified: systemd 259.5 can stall
  before login under QEMU TCG. Consequently, concurrent Ubuntu/FreeBSD SSH and
  the software-desktop path remain experimental in v0.2.
- Canonical VirtIO GPU and input devices are future work.
- FreeBSD acceptance uses read-only live media and an ephemeral SSH setup.
- The release is development evidence, not a completed production security,
  availability, side-channel, or bare-metal qualification.
