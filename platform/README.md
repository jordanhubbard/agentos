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

Do not add `net_virt` to `IMAGES` / `system_desc` until a nic_drv owns the
host NIC and the 2 MB region is a shared MR. This pass the VMM pumps locally.

## Residual `guest_ram` identity map

Emulated virtio-net copies and MMIO/PCI queue-ring walks go through
`aos_gpa_to_hva`. The identity map **must stay** until these are gone:

1. **QEMU virtio passthrough DMA** — host virtio-net/blk at `0x0A000000` /
   `0x0A000200` programs the guest GPA as a host physical address.
2. **libvmm virtio-blk / console / sound** — still cast `desc->addr` to a
   host pointer. Do not remove the map for those devices in this pass.

