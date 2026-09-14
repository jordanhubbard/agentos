# agentOS — Platform Plan

**Status:** Active
**Last updated:** 2026-09-14
**Epic:** mac `task_a2c5cdc55f994af8bc9fc48b13c54d5a` (project `agentos`)

QEMU is a hardware emulator so we can prototype quickly. agentOS is the
platform that will run on bare metal. Guests (Linux, FreeBSD) consume
**emulated virtio** served by user-mode virtualizers. Native agents consume
the same virtualizers without a guest OS.

This file sequences the active platform implementation. Release outcomes and
cross-release dependencies, including the network desktop proof, canonical
display path, x86 VMM, and stable qualification, live in
[`docs/ROADMAP.md`](docs/ROADMAP.md). Release state and ownership remain in
MAC; roadmap prose is not a substitute for task state.

The previous 6-phase plan (UI deletion, opcode contracts, AgentFS `/devices`
binding) described the wrong I/O model. It is superseded by this document.

## Priority order (do not skip)

| Step | mac task | Status | Work |
|------|----------|--------|------|
| 1 | (done) | done | TCB page + constitution rewrite |
| 2 | (done) | done (host-tested) | sDDF net under VMM (`virtio_mmio_net_init`), not QEMU passthrough |
| 2b | `task_0d44a94246554eeabc8d5bc8e36ab6d7` | done | `make test-guest-net`: boot buildroot, enumerate IPA `0x0A010000`, pump one frame |
| 3 | `task_892273845b0949ce8be59f70c02bf644` | done | `make test-guest-blk`: boot buildroot, enumerate IPA `0x0A020000`, pump one request |
| 4 | `task_9218737eb11a438b89552c599c25d012` (historical; retired in MAC) | implemented; PR #138 | Separate `serial_virt` PD, isolated client pages and bidirectional virtio-console; `serial_pd` owns the UART |
| 5 | `task_7f6653b7dcc840b9ab7fa092685c9d57` | implemented; dual-guest proof retained | Versioned profiles drive artifact acquisition, lifecycle type, boot, devices, the receive loop, bounded console state machines, and host launch tooling |
| 6 | `task_c03b1c0527de416fbcfcdfcb77787559` (historical; retired in MAC) | implemented for Linux and FreeBSD guest RAM | Bounds-checked GPA translation into disjoint VMM memory windows; guest resource reclamation remains separate work |
| 7 | (done) | done (quarantine by docs) | Quarantine PD museum (no deletes this pass) |
| 8 | (done) | done | Text-only skills + Rust helper tools |
| 9 | `task_ec992e5743354a538d1c3235a2e2c0da` | implemented; PR #140; target proof retained | Native Rust execution and isolated raw-network virtualizer client |

PR #138 merged as `e1d4ba611d2ae84c168096f58268a1774d7dcd5b`.
Its retained dual-guest qualification and remaining lifecycle limits are
documented in [`docs/TCB.md`](docs/TCB.md). Historical task retirement does
not qualify new features. Native runtime task
`task_3d190486ab18c12663a2d724bb602778` covers agentOS Rust PD execution and
integrated virtualizer networking; the migrated external RCC service-port
requirement was removed following the user's scope correction. No TokenHub or
SquirrelBus port is part of this plan.

PR #140 merged as `09fa6e40777fd81d5e87701acecfe2ec84d5935b`.
Its reviewed head `dda2421` passed the native seL4 runtime/NIC proof, the
full local gate, and all hosted checks, including native network isolation
fault probes. The retained live-Ubuntu image at `39c4f8bb` passed authenticated
SSH and three fresh native ARP batches interleaved with guest network probes.
The runtime provides bounded allocation, cooperative execution and raw Ethernet
queues; a bidirectional application service bridge and a native TCP/IP stack
are not established by that proof. MAC retains the publication evidence;
normal task closure is currently rejected while the tasks remain open and
fleet dispatch stays paused.

## Proof policy (unchanged)

Host-only tests (`make test-host`) are a pre-filter. They are **not** proof of
production IPC or I/O. `make test-host` also runs `make lint-source`, a source
lint over headers, the compiled topology, and guest FDTs (see
`tests/TARGET_TESTS.md`); it is a policy check, not a test, and is not counted
as guest-path coverage. Infrastructure claims require `make gate` (both target
arches under QEMU with `GUEST_OS=none`, plus `gate-guest-io`: the buildroot
net and blk proofs and the Ubuntu console proof). `GUEST_OS=none` on its own
is a stub VMM and proves PD load only. Guest release claims additionally require
`make demo-test`, which boots Ubuntu and FreeBSD concurrently and proves
key-only SSH to both. A device-class claim also needs its focused guest I/O
assertion through the virtualizer — not QEMU bus ownership.

The canonical dual-guest gate is the root `make demo-test` target: one QEMU,
one agentOS image, CC-PD/vm_manager guest creation, and authenticated SSH on
ports 12222 and 12223. Host QEMU buses 8, 16, and 31 are owned by canonical
agentOS driver services; neither guest receives those transport pages or IRQs.
Buildroot provides focused I/O proofs through the net and block virtualizers
(`make test-guest-net`, `make test-guest-blk`).

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
3. Buildroot and Ubuntu DTBs advertise only this emulated NIC; no Linux VMM
   maps the QEMU first virtio-mmio page.
4. Full guests use the separate `net_virt` PD over isolated client queue pages.
   Only `net_virt` holds the `net_pd` raw-frame endpoint and driver-transfer
   page. VMMs hold neither direct driver authority nor another client's page.

## First blk vertical slice (step 3)

1. Guest IPA `0x0A020000` is an **emulated** virtio-mmio blk device (fault to VMM).
2. Backend is sDDF-shaped queues into the separate `blk_virt` PD. Only that
   virtualizer calls `virtio_blk`, the driver that owns QEMU bus.8 and DMA
   memory. VMMs map only their own queue page. Buildroot retains the 256 KB
   RAM fallback.
3. Buildroot, Ubuntu, and FreeBSD advertise **only** emulated net + emulated
   block devices. Host buses 8 and 31 remain private backing transports owned
   by agentOS services.
4. Linux `virtio_blk` probe + partition scan reads the real Ubuntu ISO through
   guest emulation → sDDF pump → `virtio_blk` → host device. The runtime gate
   requires an explicit host-media read marker.
5. Payload copies use bounds-checked GPA translation. Linux and FreeBSD guest
   VSpaces both see GPA `0x40000000`, backed by disjoint VMM HVA windows.

## First serial vertical slice (step 4)

1. Ubuntu guest IPA `0x0A030000` is an emulated virtio-console (SPI 21,
   INTID 53) backed by sDDF byte queues.
2. `console=hvc0` makes the agentOS device the usable console; PL011 is
   earlycon only.
3. `make test-guest-console` requires Ubuntu's login prompt plus echoed
   input over CC-PD and VMM probe / DRIVER_OK / bidirectional pump markers.
4. Virtio-console descriptor payloads use bounds-checked GPA translation.
5. `serial_virt` now multiplexes separate VMM pages and a CC frontend page.
   Persistent notifications wake queue consumers. `serial_pd` is the sole
   post-bootstrap UART owner; generic log-ring provisioning remains separate
   work under `task_d41eae5495924820bc2defa15750d4e8`.

## Ubuntu all-VirtIO gate

`make test-ubuntu-virtio` uses QEMU NIC and block devices as hardware stand-ins
owned by agentOS driver PDs. The guest DTB advertises only agentOS emulated net (`0x0A010000`),
block (`0x0A020000`), and console (`0x0A030000`). The gate requires all three
to probe, reach DRIVER_OK, and transfer real guest I/O before accepting a login
and echoed input over CC-PD. CI runs the same gate.

`make test-ubuntu-live QEMU_TEST_TIMEOUT=1500` additionally loads Ubuntu's real
Casper initrd from the agentOS-owned ISO, mounts the live filesystem, reaches
an authenticated `ubuntu` shell over emulated virtio-console, and emits a
bounded guest network probe. The gate rejects initramfs unpack failures and
requires probe, DRIVER_OK, and real I/O markers for net, block, and console.
CI runs the deterministic initramfs gate on every push; the full-live gate
runs nightly and on demand (`ubuntu-live-nightly.yml`) because it takes up to
two hours under TCG on hosted runners.

This closes the full Ubuntu live-filesystem proof. Ubuntu retains the same
emulated-only DTB, translated RAM, and agentOS-owned bus.8 backend in a dual
image. The dual authenticated-SSH acceptance path is `make demo-test`;
`make demo` runs the same gate and retains both guests for manual sessions.
