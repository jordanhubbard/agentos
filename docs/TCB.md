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
        │                  relays through it
        ├── net_pd         owns QEMU virtio-net (bus.16, IPA 0x0A002000)
        ├── virtio_blk     owns QEMU virtio-blk (bus.8, IPA 0x0A001000) and
        │   / block_pd     the bounded DMA window
        ├── vm_manager     guest lifecycle control (create, bind, status)
        └── guest_vmm_*    vCPU, vGIC, emulated virtio-mmio net/blk/console,
              │            GPA-translated payload copies
              ├── Linux guest    in-tree virtio drivers
              └── FreeBSD guest  in-tree virtio drivers
```

**How I/O flows today.** The virtualizer is a *library* linked into each
`guest_vmm` PD (`platform/net-virt/vmm_virtio_net.c`,
`platform/blk-virt/vmm_virtio_blk.c`, `platform/serial-virt/vmm_virtio_console.c`).
The sDDF-shaped queues sit between the emulated device and a pump inside the
VMM address space. From the pump, net frames reach `net_pd` by per-frame seL4
IPC with a shared data slot, block requests reach `virtio_blk` by IPC chunked
through the DMA window, and console bytes reach `cc_pd` by IPC. There is no
`serial_virt`, `net_virt`, or `blk_virt` PD in the image;
`platform/net-virt/net_virt.c` and `platform/blk-virt/blk_virt.c` are not
compiled by any build rule.

That per-request IPC violates invariant 2 below. It is recorded here so the
gap is visible, not to license it. Closing it is MAC
`task_2895878a309f431da2d082d75c93e20d`.

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
   `MSG_NET_SEND` through IPC registers. *Not yet held* (see above).
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
spawns exactly the PDs in `src/system_desc_aarch64.c` (19 in the default
image; `guest_vmm_secondary`, `fault_inject`, and `test_runner` are added only
to the image variants that use them), and `agentos.toml` now lists that same
set and nothing else (MAC `task_56eae59d9aa94d2d9d047f03fc9d22ad`; it
previously bundled 39 ELFs, 20 of which the root task never started). Museum
sources are still compiled by the root-task Makefile `IMAGES` list so they keep
building, but they are not in the image. Of the 19 booted PDs, `agentfs`,
`vibe_engine`, `vfs_server`, `net_server`, `framebuffer_pd`, `usb_pd`,
`event_bus`, `init_agent`, and `controller` are not TCB: they stay because the
descriptor, `cc_pd` (which relays dynamic-guest control to `vibe_engine`), and
the `controller` boot sequence that prints `agentOS boot complete` still
resolve them. Removing them from the descriptor is a follow-up, not part of
this manifest trim.

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
