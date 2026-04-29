# P0 Guest E2E Gap Tasks

This file tracks the gaps found in the P0 guest proof audit. The original
acceptance target is: multiple different OS environments boot to multi-user,
accept an incoming SSH session, and use shared generic device PDs for networking,
block I/O, framebuffer, serial, USB, and related device classes.

Status values: `todo`, `in-progress`, `blocked`, `done`.

## P0-GUEST-001: Make the Linux guest VMM build path deterministic

Status: `done`

Gap: the opt-in Linux/Ubuntu guest build can reuse a stale default-stub
`linux_vmm.o` in `build/<board>/`, then link it through the full libvmm path.
Observed failure:

```text
ld.lld: error: undefined symbol: __sel4_ipc_buffer
```

Acceptance:

- `make build BOARD=qemu_virt_aarch64 TARGET_ARCH=aarch64 GUEST_OS=ubuntu`
  builds `linux_vmm.elf` from a full libvmm object, not the default stub object.
- Rebuilding after a default `GUEST_OS=none` build does not reuse stale VMM
  objects compiled with incompatible flags.

Execution notes:

- `make build BOARD=qemu_virt_aarch64 TARGET_ARCH=aarch64 GUEST_OS=ubuntu`
  passes.
- A `GUEST_OS=none` build followed by `GUEST_OS=ubuntu` relinks
  `linux_vmm.elf` from `linux_vmm.full.o` rather than the default stub object.
- The full Linux VMM ELF now includes `_start` and reports start address
  `0x200000` instead of entry `0x0`.

## P0-GUEST-002: Make the FreeBSD guest VMM build path buildable

Status: `blocked`

Gap: the opt-in FreeBSD path currently fails before producing
`freebsd_vmm.elf`. Observed failure:

```text
kernel/freebsd-vmm/vmm.c:26:10: fatal error: 'sel4/sel4.h' file not found
```

Acceptance:

- `make build BOARD=qemu_virt_aarch64 TARGET_ARCH=aarch64 GUEST_OS=freebsd`
  builds `freebsd_vmm.elf`.
- The FreeBSD build path is singular and documented. Competing FreeBSD VMM
  implementations must be reconciled or explicitly retired.

Execution notes:

- The missing SDK include path was fixed, exposing the next real blocker in
  `kernel/freebsd-vmm`: it calls stale/nonexistent local libvmm APIs
  (`vm_init`, `vm_run`, `vm_suspend`, `vm_resume`, `vm_handle_fault`) and uses
  an incompatible `vcpu_regs_t` layout.
- There is still a competing FreeBSD implementation in
  `kernel/agentos-root-task/src/freebsd_vmm.c`; the active top-level build path
  delegates to `kernel/freebsd-vmm`, so this remains blocked until the project
  picks one implementation and retires or ports the other.
- The alternate `vmm.mk` path now clean-builds `freebsd_vmm.elf` directly:
  `make -B -f vmm.mk ... GUEST_OS=freebsd vmm-all` passes. It is not yet wired
  into the top-level image/runtime path, and the AArch64 system description
  still starts `linux_vmm` rather than `freebsd_vmm`.

## P0-GUEST-003: Make the E2E harness truthful for guest SSH

Status: `done`

Gap: `tests/e2e/run_e2e.sh` currently treats missing SSH as a skip after the
guest marker check, and child module failures do not reliably increment the
parent failure count.

Acceptance:

- Without an explicit override, missing guest SSH is a hard E2E failure.
- A failed child E2E module increments the parent failure count and exits
  non-zero.
- A passing run must include the portable SSH suite from
  `tests/e2e/suite_common.sh`.

Execution notes:

- The harness now starts QEMU on macOS without `setsid`, captures serial output
  using `ncat --unixsock --recv-only` when available, and fails if the guest
  boot marker is absent.
- Child module failures now increment the parent failure count.
- Validated with:
  `E2E_TIMEOUT=20 E2E_SKIP_BRIDGE=1 E2E_GUEST_OS=ubuntu-arm64 E2E_CC_PORT=18789 E2E_SSH_PORT=12222 bash tests/e2e/run_e2e.sh`.
  Result: agentOS booted, Ubuntu did not reach `login:`, and the suite exited
  non-zero with two failures.

## P0-GUEST-004: Align CC consumer tests with the implemented CC transport

Status: `todo`

Gap: the E2E shell tests expect HTTP endpoints at `/api/agentos/cc/*`, while
the implemented host consumer uses a Unix socket binary frame via `agentctl`.

Acceptance:

- E2E CC tests use the real `agentctl`/Unix socket transport or a checked-in
  host bridge that is built and launched by the harness.
- Guest/device/framebuffer lifecycle tests do not silently skip because a
  non-existent HTTP bridge is unavailable.

## P0-GUEST-005: Replace block_pd stub forwarding with real virtio_blk IPC

Status: `todo`

Gap: `block_pd` advertises forwarding to `virtio_blk`, but its implementation
uses local stub message buffers instead of a real service call.

Acceptance:

- `MSG_BLOCK_OPEN/READ/WRITE/FLUSH/STATUS` call the real `virtio_blk` PD.
- Contract tests fail if the backend call path is stubbed.
- A guest root filesystem read/write test over SSH proves the path.

## P0-GUEST-006: Prove guest VMMs bind to shared generic device PDs

Status: `todo`

Gap: the Linux VMM binding protocol is still partially TODO, and FreeBSD has
competing implementations with inconsistent device paths.

Acceptance:

- Linux and FreeBSD VMMs complete the guest binding protocol before guest boot.
- Device handles for serial, net, block, framebuffer, and USB are opened through
  generic PD contracts, not guest-specific direct hardware mappings.
- E2E logs expose enough structured evidence to prove which device handles each
  guest received.

## P0-GUEST-007: Include a real USB PD in the boot image

Status: `todo`

Gap: the build currently includes `usb_svc.elf`, which is a stub. The real
`usb_pd.c` implementation is not built into the image as `usb_pd.elf`.

Acceptance:

- The image contains `usb_pd.elf`.
- USB contract tests query the real USB PD.
- The guest E2E suite either verifies USB enumeration through SSH or records an
  explicit hardware-waiver issue.

## P0-GUEST-008: Add a multi-OS guest E2E matrix

Status: `todo`

Gap: no current CI/local command proves multiple OS families reach multi-user
and accept SSH.

Acceptance:

- One command runs the matrix for at least FreeBSD and Linux/Ubuntu on
  `qemu_virt_aarch64`.
- Each matrix entry requires: agentOS boot marker, guest login or multi-user
  marker, successful SSH command, network sanity, block write/read sanity.
- The matrix fails if any OS is skipped unless the skip is explicitly requested
  by an environment variable documented in the test output.

## P0-GUEST-009: Make Linux/Ubuntu reach guest login and SSH

Status: `done`

Gap: after the Linux VMM build path became deterministic and the ELF started at
a valid entry point, the guest still did not reach a usable login or SSH session
under `linux_vmm`.

Acceptance:

- `cargo xtask qemu-test --no-build --guest-os ubuntu --timeout-secs <N>`
  observes an Ubuntu guest boot marker before timeout.
- `tests/e2e/run_e2e.sh` reaches the guest SSH check and completes a command
  through the forwarded SSH port.
- The serial log identifies the guest VMM startup path and device bindings
  clearly enough to distinguish a guest boot failure from an agentOS boot
  failure.

Execution notes:

- Fixed the prior hard stop where `linux_vmm` treated non-fault IRQ/notification
  traffic as a guest VCPU fault. The VMM now only enters `fault_handle()` for
  badges carrying the VMM fault-badge flag; IRQ notifications are dispatched
  as notifications even if seL4 supplies a non-null label.
- Added a host regression in `tests/api/test_irq_setup.c` for both sides of
  the bug: IRQ badge `0x2` no longer falls through to the controller channel,
  and a non-fault label with IRQ badge `0x4` is not dispatched as a VMM fault.
- Added NoCloud-Net seeding to `xtask qemu-test` and the E2E harness. The
  Ubuntu DTB now passes
  `ds=nocloud-net;s=http://10.0.2.2:18790/`, and `xtask` boots the cached clean
  Ubuntu image with QEMU snapshot writes to avoid contaminated disk state.
- Direct clean QEMU control passes with the same kernel, clean image, NoCloud
  seed, and SSH key:
  `SSH_UBUNTU_OK at 80s`.
- The remaining `linux_vmm` clean first-boot SSH blocker was fixed in
  P0-GUEST-010. `cargo xtask qemu-test --no-build --guest-os ubuntu
  --timeout-secs 1200` now passes by completing an SSH command that checks
  `multi-user.target`.

Current blocker:

- No remaining Ubuntu boot-to-multi-user SSH blocker is known. The remaining P0
  gaps are broader coverage gaps: FreeBSD runtime proof, the real CC transport,
  real USB PD coverage, and proof that every guest device path is bound through
  generic PD contracts rather than only through QEMU/virtio evidence.

## P0-GUEST-010: Fix clean first-boot Ubuntu SSH under linux_vmm

Status: `done`

Gap: Direct QEMU proves the clean Ubuntu image plus NoCloud-Net seed accepts
an incoming SSH command, but the same clean image and seed under AgentOS
`linux_vmm` does not complete SSH within 1200 seconds.

Acceptance:

- A clean/snapshot Ubuntu image under `linux_vmm` completes
  `ssh <user>@127.0.0.1 systemctl is-active --quiet multi-user.target` through
  QEMU hostfwd without relying on a previously mutated guest disk.
- The proof log includes NoCloud fetches, root filesystem mount, network IRQs,
  and the successful SSH marker.
- The fix identifies whether the failure was VMM timer/VPPI behavior, virtio
  networking, cloud-init progression, or SSH key application.

Execution notes:

- Root causes:
  - Trapped AArch64 WFI/WFE faults were returned as handled without advancing
    the vCPU PC, allowing an idle guest to spin on the same faulting
    instruction.
  - The Rust `xtask qemu-test` NoCloud user-data string used escaped newlines in
    a way that stripped indentation under `write_files.content`, so cloud-init
    rejected the YAML and did not reliably install the SSH key on clean first
    boot.
  - Reused NoCloud `instance-id` values could hide seed changes behind
    cloud-init's per-instance semaphores.
- Fixes:
  - `libvmm/src/arch/aarch64/fault.c` now advances the vCPU for trapped
    WFI/WFE and throttles VPPI diagnostic logging.
  - `xtask/src/cmd_test.rs` now serves valid NoCloud user-data from a raw string,
    uses a unique instance-id per run, boots Ubuntu with snapshot writes, and
    treats success as SSH completion of
    `systemctl is-active --quiet multi-user.target`.
  - `tests/e2e/run_e2e.sh` mirrors the unique NoCloud instance-id behavior and
    requires Ubuntu's SSH check to prove `multi-user.target`.
  - `tests/e2e/test_device_contracts.sh` uses the selected SSH user and bounds
    guest SSH probes so optional USB enumeration cannot hang the full suite.
- Focused proof:
  `cargo xtask qemu-test --no-build --guest-os ubuntu --timeout-secs 1200`
  passed. `/tmp/agentos-ubuntu-qemu-test.log` shows NoCloud GETs for
  `/meta-data`, `/user-data`, and `/vendor-data`, root mounted on `253:1`, and:

```text
PASS [board=qemu_virt_aarch64]: found marker "ssh root@127.0.0.1:12222 systemctl is-active --quiet multi-user.target"
```

- Broad Ubuntu E2E proof:
  `E2E_TIMEOUT=480 E2E_SKIP_BRIDGE=1 E2E_GUEST_OS=ubuntu-arm64
  E2E_CC_PORT=18789 E2E_SSH_PORT=12222 bash tests/e2e/run_e2e.sh` passed.
  `/tmp/agentos-e2e-ubuntu.log` shows:

```text
[PASS] ubuntu-arm64 VM (slot 0) booted to SSH-ready multi-user
[PASS] ubuntu-arm64 SSH reachable on port 12222
[PASS] serial: Ubuntu console emitted kernel and systemd boot logs
[PASS] net: SSH port 12222 reachable from host
[PASS] block: disk I/O round-trip (write→read verified)
[PASS] Guest SSH Command Suite
[PASS] E2E suite: all 6 test(s) passed
```
