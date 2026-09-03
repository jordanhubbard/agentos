# agentOS — Platform Plan

**Status:** Active  
**Last updated:** 2026-09-02  
**Epic:** mac project `agentos` (former beads `agentos-kpq`)

QEMU is a hardware emulator so we can prototype quickly. agentOS is the
platform that will run on bare metal. Guests (Linux, FreeBSD) consume
**emulated virtio** served by user-mode virtualizers. Native agents consume
the same virtualizers without a guest OS.

The previous 6-phase plan (UI deletion, opcode contracts, AgentFS `/devices`
binding) described the wrong I/O model. It is superseded by this document.

## Priority order (do not skip)

| Step | Former beads | Status | Work |
|------|--------------|--------|------|
|------|-------|--------|------|
| 1 | `agentos-mzl` | done | TCB page + constitution rewrite |
| 2 | `agentos-f6y` | done (host-tested) | sDDF net under VMM (`virtio_mmio_net_init`), not QEMU passthrough |
| 2b | `agentos-bp0` | open | Guest must enumerate IPA `0x0A010000` and pass a packet through the pump |
| 3 | `agentos-328` | blocked on 2 guest proof | sDDF blk + emulated virtio-blk |
| 4 | `agentos-9sc` | blocked on 2 guest proof | sDDF serial; one UART owner |
| 5 | `agentos-pux` | blocked on 2 guest proof | One VMM implementation; guest flavor is data |
| 6 | `agentos-4yf` | blocked on 2 guest proof | Stop identity-mapping guest RAM for device DMA |
| 7 | `agentos-vck` | done (quarantine by docs) | Quarantine PD museum (no deletes this pass) |
| 8 | `agentos-anr` | done | Skills + Python HTML helpers |
| 9 | `agentos-mq0` | blocked on 2 guest proof | Native agent services as virtualizer clients |

## Proof policy (unchanged)

Host-only tests (`make test-host`) are a pre-filter. They are **not** proof of
production IPC or I/O. OS-level claims require `make gate` (both target
arches under QEMU) plus, for a device class, a guest I/O assertion through
the virtualizer — not QEMU bus ownership.

Dual-guest E2E remains a **guest-boot** gate until emulated virtio-net carries
packets. Then it must assert I/O through `net_virt`, not QEMU bus ownership.

Host tests for `aos_net_virt_pump` are a pre-filter. They are not proof that
the guest sees the device. That proof is `agentos-bp0`.

## First net vertical slice (step 2)

1. Guest IPA `0x0A010000` is an **emulated** virtio-mmio net device (fault to VMM).
2. Backend is sDDF-shaped queues + `aos_net_virt_pump` (loopback / hub).
3. Guest DTB advertises this device. QEMU virtio-net at `0x0A000000` stays as
   a kill-dated crutch so existing SSH E2E does not go dark in this pass.
4. Next: host virtio-net MMIO moves to a nic_drv PD; passthrough is removed;
   both guests share one `net_virt`.
