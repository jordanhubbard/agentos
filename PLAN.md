# agentOS — Platform Plan

**Status:** Active  
**Last updated:** 2026-09-03  
**Epic:** mac `task_a2c5cdc55f994af8bc9fc48b13c54d5a` (project `agentos`)

QEMU is a hardware emulator so we can prototype quickly. agentOS is the
platform that will run on bare metal. Guests (Linux, FreeBSD) consume
**emulated virtio** served by user-mode virtualizers. Native agents consume
the same virtualizers without a guest OS.

The previous 6-phase plan (UI deletion, opcode contracts, AgentFS `/devices`
binding) described the wrong I/O model. It is superseded by this document.

## Priority order (do not skip)

| Step | mac task | Status | Work |
|------|----------|--------|------|
| 1 | (done) | done | TCB page + constitution rewrite |
| 2 | (done) | done (host-tested) | sDDF net under VMM (`virtio_mmio_net_init`), not QEMU passthrough |
| 2b | `task_0d44a94246554eeabc8d5bc8e36ab6d7` | done | `make test-guest-net`: boot buildroot, enumerate IPA `0x0A010000`, pump one frame |
| 3 | `task_892273845b0949ce8be59f70c02bf644` | done | `make test-guest-blk`: boot buildroot, enumerate IPA `0x0A020000`, pump one request |
| 4 | `task_9218737eb11a438b89552c599c25d012` | in progress | Ubuntu hvc0 uses emulated virtio-console + sDDF queues; remove residual direct UART ownership |
| 5 | `task_7f6653b7dcc840b9ab7fa092685c9d57` | waiting on 4 | One VMM implementation; guest flavor is data |
| 6 | `task_c03b1c0527de416fbcfcdfcb77787559` | waiting on 4 | Stop identity-mapping guest RAM for device DMA |
| 7 | (done) | done (quarantine by docs) | Quarantine PD museum (no deletes this pass) |
| 8 | (done) | done | Skills + Python HTML helpers |
| 9 | `task_ec992e5743354a538d1c3235a2e2c0da` | waiting on 4 | Native agent services as virtualizer clients |

## Proof policy (unchanged)

Host-only tests (`make test-host`) are a pre-filter. They are **not** proof of
production IPC or I/O. OS-level claims require `make gate` (both target
arches under QEMU) plus, for a device class, a guest I/O assertion through
the virtualizer — not QEMU bus ownership.

Dual-guest E2E remains a **guest-boot** gate until emulated virtio-blk is
the Ubuntu boot disk. Buildroot already proves I/O through `net_virt` and
`blk_virt` (`make test-guest-net`, `make test-guest-blk`).

Host tests for `aos_net_virt_pump` / `aos_blk_virt_pump` are a pre-filter.
They are not proof that the guest sees the device. That proof is
`make test-guest-net` / `make test-guest-blk`.

## Operator session (parallel; not I/O proof)

Hermes-on-agentOS is a **client** of inspect + later `serial_virt`, not a PD
and not `term_server`. Start with a read-only snapshot of memory, threads,
and hardware (`platform/include/platform/inspect.h`). Host tests are a
pre-filter. They are not a live seL4 query.

| Step | mac task | Status | Work |
|------|----------|--------|------|
| A | `task_72e781c303084d638b732e48d0e9132d` | open | Epic: inspect, then Hermes client |
| A1 | `task_a80ae509b10540c7932b03d95df2e74b` | open | Packed inspect snapshot + structured report |
| A2 | `task_0981068853cc4881886a6483f1583733` | waiting on A1 | Line protocol on `serial_virt` |
| A3 | `task_1ab2cbb61c374bd99b43bbfbebf05bdc` | waiting on A1 | Guest/external Hermes; user API key; never in-tree |

Session context: `skills/hermes-session/SKILL.md`. Compose/mutate of
services is out of scope until inspect and serial attach exist.

## First net vertical slice (step 2)

1. Guest IPA `0x0A010000` is an **emulated** virtio-mmio net device (fault to VMM).
2. Backend is sDDF-shaped queues + `aos_net_virt_pump` (loopback / hub).
3. Guest DTB advertises this device. Buildroot overlay has **only** the
   emulated NIC (`make test-guest-net`). Ubuntu overlays still carry QEMU
   virtio-net at `0x0A000000` as a kill-dated SSH crutch until nic_drv
   exists.
4. Next: host virtio-net MMIO moves to a nic_drv PD; passthrough is removed
   from Ubuntu too; both guests share one `net_virt`.

## First blk vertical slice (step 3)

1. Guest IPA `0x0A020000` is an **emulated** virtio-mmio blk device (fault to VMM).
2. Backend is sDDF-shaped queues + `aos_blk_virt_pump` (256 KB RAM disk).
3. Buildroot and the single-guest Ubuntu E2E overlay advertise **only**
   emulated net + emulated blk. Dual-guest mode still retains the QEMU ISO
   crutch until the host block driver can serve live media to `blk_virt`.
4. Linux `virtio_blk` probe + partition scan of LBA 0 is the I/O that
   proves the pump. Do not set `root=` to the RAM disk.
5. Payload copies in libvmm `block.c` still cast `desc.addr` as a host
   pointer — identity map of guest RAM stays until step 6.

## First serial vertical slice (step 4)

1. Ubuntu guest IPA `0x0A030000` is an emulated virtio-console (SPI 21,
   INTID 53) backed by sDDF byte queues.
2. `console=hvc0` makes the agentOS device the usable console; PL011 is
   earlycon only.
3. `make test-guest-console` requires Ubuntu's login prompt plus echoed
   input over CC-PD and VMM probe / DRIVER_OK / bidirectional pump markers.
4. Virtio-console descriptor payloads use bounds-checked GPA translation.
5. Remaining before step 4 closes: remove direct UART mappings from every
   non-driver PD so `serial_pd` is the sole post-bootstrap hardware owner.

## Ubuntu all-VirtIO gate

`make test-ubuntu-virtio` launches no QEMU net or block device for the single
Ubuntu guest. The DTB advertises only agentOS emulated net (`0x0A010000`),
block (`0x0A020000`), and console (`0x0A030000`). The gate requires all three
to probe, reach DRIVER_OK, and transfer real guest I/O before accepting a login
and echoed input over CC-PD. CI runs the same gate.

This currently proves the Ubuntu kernel plus deterministic E2E initramfs. It
does **not** yet prove the full Ubuntu live filesystem from the ISO. That
requires the QEMU host block device to be owned by the agentOS block driver
and served through `blk_virt`; do not call the platform pivot complete before
that live-media boot passes.
