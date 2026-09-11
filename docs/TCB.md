# agentOS Trusted Computing Base

**This page is binding.** If a component is not listed here, it is not trusted
and must not own a device frame, an IRQ, or guest RAM. QEMU is a hardware
emulator for prototyping. On a board, the same PDs own the real devices.

This page describes two things and keeps them apart: what **boots today**
(verifiable against `kernel/agentos-root-task/agentos.toml`) and the **target
shape**. A claim that belongs to the target column is not an OS claim until the
manifest and the gate agree with it.

## Privilege

| Level | What runs | Notes |
|-------|-----------|--------|
| EL2 / seL4 | seL4 microkernel only | Never modified. Caps, IPC, scheduling, VMX/VHE. |
| EL0 (agentOS PDs) | Everything we write | User mode. No PD is "ring 1–5". |
| EL1 (guest kernel) | Linux, FreeBSD | Hostile. Speak virtio. Never see host MMIO. |
| EL0 (guest user) | Guest userspace | Guest VSpace, not the VMM's. |

Intel "rings" are not ARM exception levels. Do not document agentOS that way.

## TCB today — what the boot manifest actually gives device or guest caps

These PDs hold device frames, IRQs, or guest-execution caps in the image that
`make gate` boots. Names are the `agentos.toml` PD names.

```
seL4
  └── root task            untyped, CSpace, VSpace, spawn PDs, hand out caps,
      (~6 kLOC)            then parks in seL4_Wait; no post-spawn policy
        ├── serial_pd      owns the PL011 UART frame + IRQ
        ├── cc_pd          owns QEMU virtio-serial (bus.2): the console the
        │                  test harness and agentctl drive; guest console TX/RX
        │                  relays through it; prints `agentOS boot complete`
        │                  as the lowest-priority PD in the image
        ├── net_pd         owns QEMU virtio-net (bus.16, IPA 0x0A002000); its
        │                  only client is net_virt
        ├── net_virt       network virtualizer: no device frame, no IRQ; the
        │                  only net mux (sDDF queues in the shared net frame
        │                  + NBSend notifications, RAW contract into net_pd)
        ├── virtio_blk     owns QEMU virtio-blk (bus.8, IPA 0x0A001000) and
        │   / block_pd     the bounded DMA window; its only client is blk_virt
        ├── blk_virt       block virtualizer: no device frame, no IRQ; the
        │                  only blk mux (sDDF queues in the shared block
        │                  region + NBSend notifications, chunked DMA-window
        │                  Calls into virtio_blk)
        ├── vm_manager     guest lifecycle control (create, bind, status)
        └── guest_vmm_*    vCPU, vGIC, emulated virtio-mmio net/blk/console,
              │            GPA-translated payload copies
              ├── Linux guest    in-tree virtio drivers
              └── FreeBSD guest  in-tree virtio drivers
```

**How I/O flows today.**

*Network* (invariant 2 held). `net_virt` (`platform/net-virt/net_virt.c`) is
a PD of its own, spawned at priority 205 with no device frame and no IRQ. The
emulated virtio-net inside each `guest_vmm` (`platform/net-virt/vmm_virtio_net.c`,
libvmm `src/virtio/net.c`) produces and consumes sDDF-shaped queues in the
2 MB shared net frame (`AGENTOS_NET_SHARED_VA`, one 512 KB stride per guest
client) that the root task maps into every VMM and into `net_virt`. Control
is one `NET_VIRT_OP_ATTACH` Call per client; after that the VMM only
`seL4_NBSend`s `NET_VIRT_EVENT_KICK` when `tx_active` is non-empty (and
`net_virt` asked for kicks through the sDDF `consumer_signalled` flag), and
`net_virt` NBSends `NET_SVC_EVENT_RX_READY` when it filled `rx_active`.
`net_virt` alone speaks `net_pd`'s RAW contract (`RAW_SEND` / `RAW_RECV`
over a per-client slot in the same frame); `net_pd` NBSends `RX_READY` to
`net_virt`, never to a VMM, and no VMM holds a `net_pd` endpoint. When
`net_pd` reports no host NIC, `net_virt` wires the clients into the
sDDF-shaped hub/loopback pump instead. Contract:
`include/contracts/net_virt_contract.h`. Lint: `tests/platform/lint_source_invariants.c`
(`inv2:` network checks).

*Block* (invariant 2 held). `blk_virt` (`platform/blk-virt/blk_virt.c`) is a
PD of its own, spawned at priority 210 with no device frame and no IRQ. The
emulated virtio-blk inside each `guest_vmm` (`platform/blk-virt/vmm_virtio_blk.c`,
libvmm `src/virtio/block.c`) produces and consumes sDDF-shaped request and
response queues in the 4 MB shared block region (`AOS_BLK_SHMEM_VA`, one
stride per VMM slot) that the root task maps into every VMM and into
`blk_virt` and nothing else. Control is one `BLK_VIRT_OP_ATTACH` Call per
client, during which `blk_virt` probes the media and fills the client's sDDF
`storage_info`; after that the VMM only `seL4_NBSend`s `BLK_VIRT_EVENT_KICK`
when its request queue is non-empty (and `blk_virt` asked for kicks through
the `req_consumer_signalled` word), and `blk_virt` NBSends
`BLK_VIRT_EVENT_RESP_READY` when it queued responses. `blk_virt` alone holds
the `virtio_blk` endpoint and alone (besides the driver) maps the driver's
bounded DMA window, through which it chunks each request by Call; the VMM
maps no DMA window and holds no `virtio_blk` endpoint, so two guests cannot
race in that window. A profile that stages its initrd from media
(`INITRD_FROM_MEDIA`) has the VMM act as its own queue client before the
guest runs. When `virtio_blk` reports no media, `blk_virt` serves a
per-client RAM disk instead. Contract: `include/contracts/blk_virt_contract.h`.
Lint: `tests/platform/lint_source_invariants.c` (`inv2:` block checks).

*Console* (invariant 2 not yet held). The console virtualizer is still a
*library* linked into each `guest_vmm` PD
(`platform/serial-virt/vmm_virtio_console.c`): the sDDF-shaped queues sit
between the emulated device and a pump inside the VMM address space, and
console bytes reach `cc_pd` by IPC. There is no `serial_virt` PD in the
image. That per-byte IPC is recorded here so the gap is visible, not to
license it.

## TCB target — the shape the platform is converging on

```
seL4
  └── root task
        ├── serial_drv / nic_drv / blk_drv   one device frame + IRQ each
        ├── serial_virt / net_virt / blk_virt  separate PDs; the only mux;
        │                                      shared-memory queues + notifications
        └── vmm                              vCPU, vGIC, emulated virtio
              ├── Linux guest
              └── FreeBSD guest
```

Native agents are **clients of the virtualizers**, same as a VMM backend.
They are not in the TCB.

## I/O invariant

1. **One owner per device frame and IRQ.** Held today.
2. **Virtualizer is the only mux.** Shared-memory queues + notifications, not
   `MSG_NET_SEND` through IPC registers. *Held for network* (`net_virt` PD;
   enforced by the `inv2:` network lint checks and the host-backed
   `[net_virt] TX accepted by net_pd` / `[net_virt] RX delivered from net_pd`
   markers in `make test-ubuntu-virtio`) *and for block* (`blk_virt` PD;
   enforced by the `inv2:` block lint checks and the host-backed
   `[blk_virt] host media` / `[blk_virt] host-media read` markers in
   `make test-ubuntu-virtio`). *Not yet held for console* (see above).
3. **Virtio is the guest ABI.** Host may use virtio as the *physical* device
   (under QEMU). Guests must see a **different**, emulated virtio device
   invented by the VMM. Collapsing those two virtio worlds is a defect. Held
   today and enforced: `xtask qemu-test` fails a host-backed net proof that
   used the VMM-local loopback.
4. **Linux and FreeBSD are image + FDT.** No agentOS-specific guest drivers.
   Held today.
5. **No guest host-device passthrough.** A VMM must translate guest GPA and
   relay through the canonical driver/virtualizer path. Held today.

## What is not TCB (museum)

Do not extend these. Do not add opcodes. Do not "finish" them.

`oom_killer`, `mesh_agent`, `power_mgr`, `time_partition`, `wg_net`,
`pflocal_server`, `auth_server`, `mem_profiler`, `perf_counters`, `quota_pd`,
`watchdog`, `http_svc`, `spawn_server`, `exec_server`, `proc_server`,
`app_manager`, `app_slot`, `term_server`, `ext2fs` as a PD, `vibe_engine` /
`vibe_swap` / `swap_slot` as a path to networking or disks, `gpu_shmem` as a
guest channel before virtio-net is a backend, CapStore/MsgBus/ModelSvc/ToolSvc
as "core OS".

**Status:** as of 2026-09-10 none of these is bundled or booted. The root task
spawns exactly the PDs in `src/system_desc_aarch64.c` (13 in the default
image: `nameserver`, `log_drain`, `serial_pd`, `vibe_engine`, `virtio_blk`,
`block_pd`, `blk_virt`, `net_pd`, `net_virt`, `guest_vmm_primary`,
`vm_manager`, `cc_pd`, `fault_handler`; `guest_vmm_secondary`, `fault_inject`,
and `test_runner` + `event_bus` are added only to the image variants that use
them), and
`agentos.toml` lists that same set and nothing else (MAC
`task_56eae59d9aa94d2d9d047f03fc9d22ad` trimmed the manifest from 39 ELFs;
MAC `task_f95d118416a24fa484c2c43f0d955b56` then dropped `controller`,
`event_bus`, `init_agent`, `agentfs`, `vfs_server`, `net_server`,
`framebuffer_pd`, and `usb_pd` from the descriptor). Museum sources are still
compiled by the root-task Makefile `IMAGES` list so they keep building, but
they are not in the image. The one booted PD that is not TCB is
`vibe_engine`: `cc_pd` relays `MSG_CC_CREATE_GUEST` and the dynamic-guest
lifecycle/console opcodes to it, and it is the hop that issues
`OP_VM_CREATE`/`OP_VM_START` to `vm_manager`, so the dual-guest proof
(`make demo-test`) needs it. The boot-guest console path (`test-guest-console`,
`test-ubuntu-virtio`) does not: `cc_pd` forwards boot-guest input and drains
its console straight to `guest_vmm`. Teaching `cc_pd` to call `vm_manager`
directly, and retiring `vibe_engine`, is the remaining follow-up. The
`agentOS boot complete` marker the `GUEST_OS=none` harness waits for is now
printed by `cc_pd`, the lowest-priority PD in the image, right before it enters
its request loop. `tests/platform/lint_source_invariants.c` fails if any of
the dropped PDs reappears in the default descriptor.

## QEMU host transports

QEMU virtio devices are hardware stand-ins owned by canonical agentOS driver
PDs. The QEMU buses used for block media (8), networking (16), and the control
console (2) are not mapped or advertised to either guest. Linux and FreeBSD see
separate VMM-emulated virtio-net, virtio-blk, and virtio-console devices.

Reintroducing host transport DTB nodes, host IRQ registration, or direct guest
DMA against those QEMU devices is an architecture regression.

## Proof

`make gate` is the OS-claim gate: host suite, aarch64 and x86_64 boot with
`GUEST_OS=none`, and `gate-guest-io` (`make test-guest-net`,
`make test-guest-blk`, `make test-guest-console`). `GUEST_OS=none` alone is a
stub VMM and proves only that PDs load. `make demo-test` is the concurrent
dual-guest SSH acceptance path. The Ubuntu live-media proof is a nightly
release qualification, not a per-push gate.
