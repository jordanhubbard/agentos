# Changelog

All notable changes to agentOS are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

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
