# agentOS platform I/O

This tree is the **shared I/O platform**: queue layouts, virtualizer pumps, and
VMM glue that presents **emulated virtio** to guests.

QEMU is a hardware emulator. It is not the I/O server. Guests must not DMA to
QEMU virtio-mmio. Native agents attach to the same virtualizers as a VMM.

| Path | Role |
|------|------|
| `include/platform/net_layout.h` | sDDF-shaped net queue ABI (no seL4) |
| `include/platform/inspect.h` | Read-only memory/thread/hardware snapshot (no seL4) |
| `inspect/inspect_snapshot.c` | Fill + `key=value` report (host-testable) |
| `include/platform/guest_ram.h` | GPA→HVA translator for emulated virtio (host-testable) |
| `guest-ram/gpa_translate.c` | Bounds-checked `aos_gpa_to_hva` |
| `net-virt/net_virt_pump.c` | Hub / loopback (host-testable) |
| `net-virt/vmm_virtio_net.c` | libvmm `virtio_mmio_net_init` + after-fault pump + RAM bind |
| `net-virt/net_virt.c` | Future `net_virt` PD — **not in the live image** |
| `include/platform/blk_layout.h` | sDDF-shaped blk queue ABI (no seL4) |
| `blk-virt/blk_virt_pump.c` | RAM-disk pump (host-testable) |
| `blk-virt/vmm_virtio_blk.c` | libvmm `virtio_mmio_blk_init` + after-fault pump |
| `blk-virt/blk_virt.c` | Future `blk_virt` PD — **not in the live image** |
| `include/platform/serial_layout.h` | sDDF serial queue sizes + guest virtio-console ABI |
| `serial-virt/vmm_virtio_console.c` | libvmm virtio-console + CC-PD queue bridge |

Do not add `net_virt` to `IMAGES` / `system_desc` until a nic_drv owns the
host NIC and the 2 MB region is a shared MR. This pass the VMM pumps locally.

Do not add `blk_virt` to `IMAGES` / `system_desc`. Buildroot DTB advertises
the emulated device at `0x0A020000` (SPI 20 / INTID 52). The single Ubuntu
E2E guest also uses this RAM backend and launches with no QEMU net/blk
devices. Dual-guest and full live-media boot still retain the QEMU ISO crutch.

## Residual `guest_ram` identity map

Emulated virtio-net copies and MMIO/PCI queue-ring walks go through
`aos_gpa_to_hva`. The identity map **must stay** until these are gone:

1. **QEMU virtio passthrough DMA** — host virtio-net/blk at `0x0A000000` /
   `0x0A000200` programs the guest GPA as a host physical address.
2. **libvmm virtio-blk / sound** — still cast `desc->addr` to a
   host pointer. Do not remove the map for those devices in this pass.

Virtio-net and virtio-console descriptor payloads and all emulated virtqueue
rings already use the configured GPA-to-HVA translator.
