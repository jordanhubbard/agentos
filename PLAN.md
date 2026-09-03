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
| 3 | `task_892273845b0949ce8be59f70c02bf644` | ready | sDDF blk + emulated virtio-blk |
| 4 | `task_9218737eb11a438b89552c599c25d012` | waiting on 3 | sDDF serial; one UART owner |
| 5 | `task_7f6653b7dcc840b9ab7fa092685c9d57` | waiting on 3 | One VMM implementation; guest flavor is data |
| 6 | `task_c03b1c0527de416fbcfcdfcb77787559` | waiting on 3 | Stop identity-mapping guest RAM for device DMA |
| 7 | (done) | done (quarantine by docs) | Quarantine PD museum (no deletes this pass) |
| 8 | (done) | done | Skills + Python HTML helpers |
| 9 | `task_ec992e5743354a538d1c3235a2e2c0da` | waiting on 3 | Native agent services as virtualizer clients |

## Proof policy (unchanged)

Host-only tests (`make test-host`) are a pre-filter. They are **not** proof of
production IPC or I/O. OS-level claims require `make gate` (both target
arches under QEMU) plus, for a device class, a guest I/O assertion through
the virtualizer — not QEMU bus ownership.

Dual-guest E2E remains a **guest-boot** gate until emulated virtio-net carries
packets. Then it must assert I/O through `net_virt`, not QEMU bus ownership.

Host tests for `aos_net_virt_pump` are a pre-filter. They are not proof that
the guest sees the device. That proof is
`task_0d44a94246554eeabc8d5bc8e36ab6d7`.

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
