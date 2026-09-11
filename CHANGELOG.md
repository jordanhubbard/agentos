# Changelog

All notable changes to agentOS are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

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
- The default aarch64 image boots 11 PDs (nameserver, log_drain, serial_pd,
  vibe_engine, virtio_blk, block_pd, net_pd, guest_vmm_primary, vm_manager,
  cc_pd, fault_handler). The manifest previously bundled 39 ELFs, 20 of which
  the root task never started; the descriptor also dropped controller,
  event_bus, init_agent, agentfs, vfs_server, net_server, framebuffer_pd, and
  usb_pd. `cc_pd` now prints the `agentOS boot complete` marker.
- `docs/TCB.md` describes what boots today separately from the target shape,
  names `cc_pd` as the console driver, and records that the virtualizer is a
  library inside `guest_vmm` bridging to driver PDs by IPC (invariant 2 not yet
  held; tracked as a MAC task).
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

### Known limitations

- `net_virt` and `blk_virt` are not yet separate PDs; frames and block
  requests reach the driver PDs by per-request IPC from the VMM.
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
