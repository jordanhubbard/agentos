# FreeBSD VM Guest on agentOS/seL4

**Status:** Implementation in progress
**Date:** 2026-05-05
**Target platform:** QEMU virt AArch64 (Sparky GB10)

---

## Overview

Boot FreeBSD 15.0 as a VM guest inside agentOS, running on the seL4
microkernel as hypervisor. The active path stages FreeBSD assets into
`build/guest-images`, builds the AArch64 agentOS image, exposes the guest
through the CC-PD Unix socket at `build/cc_pd.sock`, and validates console
boot/input through `make test-guest-login`. The release-facing path is
`make demo`, which boots FreeBSD beside Ubuntu and requires authenticated SSH
to both guests before allowing manual sessions.

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    agentOS (seL4 @ EL2)                 │
│                                                         │
│  ┌─────────────────────────────────────────────┐        │
│  │  freebsd_vm PD (VMM, priority 200)          │        │
│  │                                             │        │
│  │  ┌─────────────────────────────────────┐    │        │
│  │  │  libvmm (seL4 Microkit VMM library) │    │        │
│  │  │  - GICv3 virtualisation             │    │        │
│  │  │  - vCPU management                  │    │        │
│  │  │  - MMIO fault handling              │    │        │
│  │  │  - VirtIO-net / VirtIO-blk          │    │        │
│  │  └─────────────────────────────────────┘    │        │
│  │                                             │        │
│  │  ┌─────────────────────────────────────┐    │        │
│  │  │  Direct FreeBSD kernel + FDT boot   │    │        │
│  │  │  → virtio-blk DVD ISO root          │    │        │
│  │  └─────────────────────────────────────┘    │        │
│  │                  ↕ guest RAM (512 MB)        │        │
│  │  ┌─────────────────────────────────────┐    │        │
│  │  │  FreeBSD 15.0 AArch64 guest         │    │        │
│  │  │  - jails → seL4 PD analogy          │    │        │
│  │  │  - ZFS, pf, bhyve-as-agent          │    │        │
│  │  └─────────────────────────────────────┘    │        │
│  └─────────────────────────────────────────────┘        │
│                                                         │
│  [controller] [event_bus] [worker_0..7] [agentfs] ...  │
└─────────────────────────────────────────────────────────┘
```

---

## Why This Works

seL4 runs at EL2 (ARM hypervisor mode) — it IS the hypervisor.  
The Microkit `libvmm` library provides a ready-made VMM PD that:
- Manages guest vCPU registers (ARM `seL4_ARM_VCPU_*` syscalls)
- Handles MMIO faults and emulates GIC interrupt controller
- Loads kernel images into guest RAM
- Supports VirtIO devices (block, net, console)

agentOS now boots FreeBSD directly: `make fetch-guest GUEST_OS=freebsd`
extracts `/boot/kernel/kernel` from the staged FreeBSD 15.0 ISO, `vmm.mk`
packages that kernel with an agentOS-provided FDT, and `freebsd_vmm` starts
the vCPU with `x0` pointing at the FDT. The ISO remains attached as a
virtio-blk device so FreeBSD can mount the DVD root filesystem.

---

## Current Build and Test Flow

The maintained demonstration flow is:

```bash
make setup
make demo
```

This stages FreeBSD 15.0 and Ubuntu 26.04 assets, boots both guest VMMs, and
prints the FreeBSD SSH command only after key-only authentication succeeds.
Use `make demo-test` for the same non-interactive acceptance gate.

For a lower-level standalone FreeBSD run:

```bash
make fetch-guest GUEST_OS=freebsd
make build TARGET_ARCH=aarch64 GUEST_OS=freebsd
make run GUEST_OS=freebsd
make test-guest-login
```

The dual path runs one agentOS image with both dedicated VMM PDs and 3 GB of
outer QEMU RAM. Both guests see RAM at GPA `0x40000000`; the VMMs map those
frames at separate host virtual addresses, so guest addresses are never
treated as host pointers.

## Contract Surface

FreeBSD must use the same OS-neutral contracts as every other guest:

| Area | Contract |
|------|----------|
| FreeBSD-specific VMM | `kernel/agentos-root-task/include/contracts/freebsd_vmm_contract.h` |
| Generic guest lifecycle | `kernel/agentos-root-task/include/contracts/guest_contract.h` |
| Generic VMM operations | `kernel/agentos-root-task/include/contracts/vmm_contract.h` |
| Serial console | `kernel/agentos-root-task/include/contracts/serial_contract.h` |
| Block device | `kernel/agentos-root-task/include/contracts/block_contract.h` |
| Network device | `kernel/agentos-root-task/include/contracts/net_contract.h` |
| Host bridge | `kernel/agentos-root-task/include/contracts/cc_contract.h` |

---

## Boot Sequence (detailed)

```
seL4 boots → agentOS Microkit init
  → freebsd_vmm PD starts
  → freebsd_vmm copies the FreeBSD kernel to guest RAM
  → freebsd_vmm copies the direct-boot FDT near the top of guest RAM
  → seL4_ARM_VCPU_Run → guest starts at the FreeBSD kernel entry
  → FreeBSD reads /chosen/bootargs from the FDT
  → FreeBSD mounts the attached 15.0 DVD ISO over virtio-blk
  → FreeBSD boots in guest (EL1), agentOS continues at EL2
```

---

## What We Need to Build

| Component | Status | Notes |
|-----------|--------|-------|
| FreeBSD 15.0 asset staging | Wired | `make fetch-guest GUEST_OS=freebsd` |
| Top-level build/run targets | Wired | `make build TARGET_ARCH=aarch64 GUEST_OS=freebsd`; `make run GUEST_OS=freebsd` |
| CC-PD host visibility | Wired | guest listing, console drain, and input path |
| E2E login/input test | Wired | `make test-guest-login` includes FreeBSD |
| Dual Linux+FreeBSD SSH demo | Wired | `make demo` retains both guests after key-only SSH succeeds; `make demo-test` is non-interactive |
| Complete VM lifecycle operations | In progress | create/destroy/suspend/resume are relayed; snapshot/restore remain structured errors |

---

## Key References

- libvmm: https://github.com/au-ts/libvmm
- libvmm manual: https://github.com/au-ts/libvmm/blob/main/docs/MANUAL.md
- FreeBSD AArch64 QEMU wiki: https://wiki.freebsd.org/arm64/QEMU
- seL4 ARM VMM tutorial: https://docs.sel4.systems/Tutorials/camkes-vm-linux.html

---

## Remaining Work

- Keep `make test-guest-login` as the serial-console gate and `make demo-test`
  as the concurrent authenticated-SSH acceptance gate.
- Expand snapshot and restore beyond their current structured error path.
- Keep all guest images, logs, sockets, and temporary artifacts under `build/`.

Demo target: `make demo` must bring up FreeBSD and Ubuntu together inside
agentOS and prove both SSH endpoints before reporting success.
