# agentOS — Platform Plan

**Status:** Active  
**Last updated:** 2026-09-02  
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
| 2b | `task_0d44a94246554eeabc8d5bc8e36ab6d7` | open | Guest must enumerate IPA `0x0A010000` and pass a packet through the pump |
| 3 | `task_892273845b0949ce8be59f70c02bf644` | waiting on 2b | sDDF blk + emulated virtio-blk |
| 4 | `task_9218737eb11a438b89552c599c25d012` | waiting on 2b | sDDF serial; one UART owner |
| 5 | `task_7f6653b7dcc840b9ab7fa092685c9d57` | waiting on 2b | One VMM implementation; guest flavor is data |
| 6 | `task_c03b1c0527de416fbcfcdfcb77787559` | waiting on 2b | Stop identity-mapping guest RAM for device DMA |
| 7 | (done) | done (quarantine by docs) | Quarantine PD museum (no deletes this pass) |
| 8 | (done) | done | Skills + Python HTML helpers |
| 9 | `task_ec992e5743354a538d1c3235a2e2c0da` | waiting on 2b | Native agent services as virtualizer clients |

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

## First net vertical slice (step 2)

1. Guest IPA `0x0A010000` is an **emulated** virtio-mmio net device (fault to VMM).
2. Backend is sDDF-shaped queues + `aos_net_virt_pump` (loopback / hub).
3. Guest DTB advertises this device. QEMU virtio-net at `0x0A000000` stays as
   a kill-dated crutch so existing SSH E2E does not go dark in this pass.
4. Next: host virtio-net MMIO moves to a nic_drv PD; passthrough is removed;
   both guests share one `net_virt`.
