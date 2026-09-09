# agentOS Trusted Computing Base

**This page is binding.** If a component is not listed here, it is not trusted
and must not own a device frame, an IRQ, or guest RAM. QEMU is a hardware
emulator for prototyping. On a board, the same PDs own the real devices.

## Privilege

| Level | What runs | Notes |
|-------|-----------|--------|
| EL2 / seL4 | seL4 microkernel only | Never modified. Caps, IPC, scheduling, VMX/VHE. |
| EL0 (agentOS PDs) | Everything we write | User mode. No PD is "ring 1–5". |
| EL1 (guest kernel) | Linux, FreeBSD | Hostile. Speak virtio. Never see host MMIO. |
| EL0 (guest user) | Guest userspace | Guest VSpace, not the VMM's. |

Intel "rings" are not ARM exception levels. Do not document agentOS that way.

## TCB — the only privileged-adjacent userland

These PDs may hold device frames, IRQs, or guest-execution caps.

```
seL4
  └── root task     untyped, CSpace, VSpace, spawn PDs, hand out caps, idle
        ├── serial_drv / serial_pd     owns UART frame + IRQ
        ├── nic_drv                    owns NIC / host virtio-net (QEMU stand-in)
        ├── blk_drv                    owns disk / host virtio-blk (QEMU stand-in)
        ├── serial_virt                mux: native clients + VMM backends
        ├── net_virt                   mux: native clients + VMM backends
        ├── blk_virt                   mux: native clients + VMM backends
        └── vmm                        vCPU, vGIC, emulated virtio-mmio/pci
              ├── Linux guest          in-tree virtio drivers
              └── FreeBSD guest        in-tree virtio drivers
```

Native agents are **clients of the virtualizers**, same as a VMM backend.
They are not in the TCB.

## I/O invariant

1. **One owner per device frame and IRQ.**
2. **Virtualizer is the only mux.** Shared-memory queues + notifications, not
   `MSG_NET_SEND` through IPC registers.
3. **Virtio is the guest ABI.** Host may use virtio as the *physical* device
   (under QEMU). Guests must see a **different**, emulated virtio device
   invented by the VMM. Collapsing those two virtio worlds is a defect.
4. **Linux and FreeBSD are image + FDT.** No agentOS-specific guest drivers.
5. **No guest host-device passthrough.** A VMM must translate guest GPA and
   relay through the canonical driver/virtualizer path.

## What is not TCB (museum)

Do not extend these. Do not add opcodes. Do not "finish" them.

`oom_killer`, `mesh_agent`, `power_mgr`, `time_partition`, `wg_net`,
`pflocal_server`, `auth_server`, `mem_profiler`, `perf_counters`, `quota_pd`,
`watchdog`, `http_svc`, `spawn_server`, `exec_server`, `proc_server`,
`app_manager`, `app_slot`, `term_server`, `ext2fs` as a PD, `vibe_engine` /
`vibe_swap` / `swap_slot` as a path to networking or disks, `gpu_shmem` as a
guest channel before virtio-net is a backend, CapStore/MsgBus/ModelSvc/ToolSvc
as "core OS".

They may remain in the tree until they are dropped from the image. Extending
them is a bug.

## QEMU host transports

QEMU virtio devices are hardware stand-ins owned by canonical agentOS driver
PDs. The current QEMU buses used for block media and networking are not mapped
or advertised to either guest. Linux and FreeBSD see separate VMM-emulated
virtio-net, virtio-blk, and virtio-console devices.

Reintroducing host transport DTB nodes, host IRQ registration, or direct guest
DMA against those QEMU devices is an architecture regression. `make demo-test`
is the concurrent guest/SSH acceptance path; the focused target proofs are
`make test-guest-net`, `make test-guest-blk`, and
`make test-guest-console`.
