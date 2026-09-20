# Changelog

All notable changes to agentOS are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- A VMX-backed x86 guest composition with pinned UEFI firmware, generated ACPI,
  bounded guest memory, and agentOS-emulated network, block and console devices.
  Retained Intel qualification covers Debian userspace, managed recreation,
  two-vCPU workloads, writable-media isolation and detached snapshots. These
  are scoped development results; final v0.4 integration remains pending.
- Live AArch64 framebuffer and input services, emulated virtio-gpu and
  virtio-input, bounded CC frame export, and keyboard/pointer delivery. The
  external `agentos_gui` consumes these contracts; no human UI is embedded in
  the OS repository.
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
  and concurrent Ubuntu/FreeBSD SSH, including FreeBSD suspend/resume.

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
