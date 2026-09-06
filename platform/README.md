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

`net_virt` is not yet a separate live PD. The VMM-local queue adapter bridges
guest traffic to the live `net_pd` through the versioned raw-frame contract
and shared region. Moving that adapter into its own PD must preserve the same
guest ABI and single-owner host-device rule.

Buildroot uses the local RAM fallback behind the emulated device at
`0x0A020000` (SPI 20 / INTID 52). Ubuntu and FreeBSD installation media attach
as host hardware owned by canonical agentOS driver PDs. Guest DTBs advertise
only agentOS-emulated devices; no host QEMU transport page or IRQ is granted to
a guest VMM.

## Guest address translation

Each guest sees RAM at the conventional QEMU-virt GPA `0x40000000`. The root
task maps the backing frames into its VMM at a distinct HVA selected by
`guest_memory_layout.h`. All emulated virtqueue ring walks and payload copies
translate GPA to HVA through the bounded guest-RAM helpers.

The dual build maps FreeBSD's 256 MB backing window at HVA `0x80000000` and
Linux's 1 GB window at HVA `0xc0000000`; both guests independently see GPA
`0x40000000`. Virtio-net, virtio-blk, virtio-console, and virtio-sound use the
configured translator rather than casting guest descriptor addresses to host
pointers.

Run `make test-guest-net`, `make test-guest-blk`, and
`make test-guest-console` for focused target proofs. Run `make demo-test` for
the concurrent Ubuntu + FreeBSD authenticated-SSH acceptance path.
