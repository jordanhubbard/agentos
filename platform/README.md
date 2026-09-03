# agentOS platform I/O

This tree is the **shared I/O platform**: queue layouts, virtualizer pumps, and
VMM glue that presents **emulated virtio** to guests.

QEMU is a hardware emulator. It is not the I/O server. Guests must not DMA to
QEMU virtio-mmio. Native agents attach to the same virtualizers as a VMM.

| Path | Role |
|------|------|
| `include/platform/net_layout.h` | sDDF-shaped net queue ABI (no seL4) |
| `net-virt/net_virt_pump.c` | Hub / loopback (host-testable) |
| `net-virt/vmm_virtio_net.c` | libvmm `virtio_mmio_net_init` + after-fault pump |
| `net-virt/net_virt.c` | Future `net_virt` PD — **not in the live image** |
| `include/platform/blk_layout.h` | sDDF-shaped blk queue ABI (no seL4) |
| `blk-virt/blk_virt_pump.c` | RAM-disk pump (host-testable) |
| `blk-virt/vmm_virtio_blk.c` | libvmm `virtio_mmio_blk_init` + after-fault pump |
| `blk-virt/blk_virt.c` | Future `blk_virt` PD — **not in the live image** |

Do not add `net_virt` to `IMAGES` / `system_desc` until a nic_drv owns the
host NIC and the 2 MB region is a shared MR. This pass the VMM pumps locally.

Do not add `blk_virt` to `IMAGES` / `system_desc`. Guest DTB still points at
QEMU virtio-blk (`0x0A000200`) so E2E boot disk is unchanged. Emulated IPA
is `0x0A020000` (SPI 20 / INTID 52).
