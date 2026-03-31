# FreeBSD VM Guest on agentOS/seL4

**Status:** Design / Implementation Plan  
**Author:** Rocky 🐿️  
**Date:** 2026-03-30  
**Target platform:** QEMU virt AArch64 (Sparky GB10)

---

## Overview

Boot FreeBSD as a VM guest inside agentOS, running on the seL4 microkernel as hypervisor.
The approach: add a `freebsd_vm` Protection Domain (PD) to agentOS using the Microkit VMM framework
(`libvmm` from au-ts), with UEFI firmware (EDK2) or U-Boot providing the boot environment
FreeBSD AArch64 requires.

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
│  │  │  UEFI firmware (EDK2 AArch64)       │    │        │
│  │  │  → loader.efi → FreeBSD kernel      │    │        │
│  │  └─────────────────────────────────────┘    │        │
│  │                  ↕ guest RAM (1–2GB)         │        │
│  │  ┌─────────────────────────────────────┐    │        │
│  │  │  FreeBSD 14.x AArch64 guest         │    │        │
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

FreeBSD AArch64 needs UEFI or U-Boot to hand it the UEFI system table pointer.
We use the pre-built `edk2-aarch64-code.fd` UEFI firmware — same as QEMU uses for FreeBSD guests — embedded as a binary in the VMM PD.

---

## Implementation Plan

### Phase 1: libvmm integration (AArch64, QEMU target)

1. **Add libvmm as a submodule**
   ```
   git submodule add https://github.com/au-ts/libvmm libs/libvmm
   ```

2. **New PD: `kernel/freebsd-vmm/`**
   - C VMM entry point using libvmm APIs
   - Loads UEFI firmware image (EDK2) + FreeBSD `bootaa64.efi` from embedded .o
   - Configures vCPU boot PC = EDK2 reset vector
   - Provides VirtIO block device backed by a agentfs region (for FreeBSD UFS/ZFS rootfs)

3. **System description: `agentos-freebsd.system`**
   ```xml
   <memory_region name="freebsd_ram" size="0x80000_000" />  <!-- 2GB guest RAM -->
   <memory_region name="freebsd_flash" size="0x4_000_000" /> <!-- 64MB UEFI flash -->
   <memory_region name="uart" size="0x1_000" phys_addr="0x9000000" />
   <memory_region name="gic_vcpu" size="0x1_000" phys_addr="0x8040000" />
   
   <protection_domain name="freebsd_vmm" priority="200">
     <program_image path="freebsd_vmm.elf" />
     <map mr="freebsd_ram" vaddr="0x40000000" perms="rw" setvar_vaddr="guest_ram_vaddr" />
     <map mr="freebsd_flash" vaddr="0x60000000" perms="rw" setvar_vaddr="guest_flash_vaddr" />
     <map mr="uart" vaddr="0x9000000" perms="rw" />
     <map mr="gic_vcpu" vaddr="0x8010000" perms="rw" />
     <virtual_machine name="freebsd" id="0">
       <map mr="freebsd_ram" vaddr="0x40000000" perms="rwx" />
       <map mr="freebsd_flash" vaddr="0x00000000" perms="rw" />
       <map mr="uart" vaddr="0x9000000" perms="rw" />
       <map mr="gic_vcpu" vaddr="0x8010000" perms="rw" />
     </virtual_machine>
   </protection_domain>
   ```

4. **Makefile target**
   ```
   make BOARD=qemu_virt_aarch64 SYSTEM=freebsd demo-freebsd
   ```

### Phase 2: VirtIO block + FreeBSD rootfs

- Embed a minimal FreeBSD 14 memstick/UFS image as a VirtIO block backend
- libvmm has virtio_blk support → FreeBSD sees a disk, runs loader.efi, boots
- FreeBSD rootfs can be small (<512MB) for demo

### Phase 3: Capability-gated integration

- Expose FreeBSD jails as agentOS PD analogues via event_bus
- Each jail gets a capability token → controller can grant/revoke
- Net: `pf` ruleset = capability policy layer
- This is the "FreeBSD jails as seL4 PD analogy" story

---

## Boot Sequence (detailed)

```
seL4 boots → agentOS Microkit init
  → freebsd_vmm PD starts
  → libvmm: copy EDK2 firmware to guest flash region (0x0000_0000)
  → libvmm: configure vCPU entry at EDK2 reset vector
  → seL4_ARM_VCPU_Run → guest executes EDK2 UEFI firmware
  → EDK2 scans VirtIO block → finds FreeBSD EFI partition
  → EDK2 loads bootaa64.efi → loads loader.efi → loads /boot/kernel/kernel
  → loader.efi passes UEFI system table ptr → FreeBSD kernel takes over
  → FreeBSD boots in guest (EL1), agentOS continues at EL2
```

---

## What We Need to Build

| Component | Status | Notes |
|-----------|--------|-------|
| `libs/libvmm` submodule | TODO | `git submodule add https://github.com/au-ts/libvmm` |
| `freebsd_vmm` PD (C) | TODO | libvmm init + UEFI load + vCPU setup |
| `agentos-freebsd.system` | TODO | Microkit system description |
| EDK2 AArch64 firmware binary | TODO | Use pre-built `edk2-aarch64-code.fd` |
| FreeBSD 14 AArch64 rootfs | TODO | Pre-built memstick image from freebsd.org |
| Makefile `freebsd` target | TODO | Build + launch |
| VirtIO block emulation | TODO | libvmm has this, needs wiring |

---

## Key References

- libvmm: https://github.com/au-ts/libvmm
- libvmm manual: https://github.com/au-ts/libvmm/blob/main/docs/MANUAL.md
- FreeBSD AArch64 QEMU wiki: https://wiki.freebsd.org/arm64/QEMU
- seL4 ARM VMM tutorial: https://docs.sel4.systems/Tutorials/camkes-vm-linux.html
- EDK2 AArch64: pkg install edk2-bhyve OR build from tianocore/edk2

---

## Timeline Estimate

- Phase 1 (libvmm + VMM PD + UEFI boot): 1–2 days
- Phase 2 (VirtIO block + full FreeBSD boot): 1 day  
- Phase 3 (jail-as-PD capability integration): 2–3 days
- Total: ~5 days to demo-worthy FreeBSD under agentOS

Demo target: `make demo-freebsd` → FreeBSD shell prompt running inside agentOS on QEMU AArch64
