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
        │                  region + persistent notifications, chunked DMA-window
        │                  Calls into virtio_blk)
        ├── vm_manager     guest lifecycle control (create, bind, status)
        ├── serial_virt    serial queue mux, no device frame or hardware IRQ;
        │                  isolated VMM pages and separate CC frontend page
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
8 MB network region (`AGENTOS_NET_SHARED_VA`). Each VMM maps only its own
2 MB client page. A third page is reserved for the native client; `net_pd`
maps only the fourth, driver-transfer page, and `net_virt` maps all four. Control
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

Network descriptors remain untrusted even within an isolated client page.
The virtualizer validates fixed-buffer alignment, offset and packet length
before constructing payload pointers in both hardware and fallback paths.
Invalid descriptors are consumed without recycling; recovery requires valid
remaining buffers or a separately coordinated client reset. Ring operations
snapshot indices, reject occupancy above the private capacity, and each pump
pass processes at most that capacity. Host tests cover wrapping offsets,
misalignment, oversized packets, corrupt occupancy and valid traffic after a
malformed descriptor; they do not constitute an on-target malicious-client proof.

*Block* (invariant 2 held). `blk_virt` (`platform/blk-virt/blk_virt.c`) is a
PD of its own, spawned at priority 210 with no device frame and no IRQ. The
emulated virtio-blk inside each `guest_vmm` (`platform/blk-virt/vmm_virtio_blk.c`,
libvmm `src/virtio/block.c`) produces and consumes sDDF-shaped request and
response queues in the 6 MB block region (`AOS_BLK_SHMEM_VA`). The root task
maps the whole region into `blk_virt`, but only one separate 2 MB client page
into each VMM. The first page holds the virtualizer's private RAM disk;
neither VMM maps it or the other client's page. Control is one `BLK_VIRT_OP_ATTACH` Call per
client, during which `blk_virt` probes the media and fills the client's sDDF
`storage_info`; after that the VMM signals the virtualizer's bound notification
when its request queue is non-empty (and `blk_virt` asked for kicks through
the `req_consumer_signalled` word), and `blk_virt` signals the owning VMM's
bound notification when it queued responses. Both directions use send-only
capabilities and retain pending wakeups until received. Preboot media staging
therefore does not depend on a later guest exit to retry a dropped event.
Receivers classify notification badges before interpreting IPC message tags.
`blk_virt` alone holds
the `virtio_blk` endpoint and alone (besides the driver) maps the driver's
bounded DMA window, through which it chunks each request by Call; the VMM
maps no DMA window and holds no `virtio_blk` endpoint, so two guests cannot
race in that window. A profile that stages its initrd from media
(`INITRD_FROM_MEDIA`) has the VMM act as its own queue client before the
guest runs. When `virtio_blk` reports no media, `blk_virt` serves a
per-client RAM disk instead. Contract: `include/contracts/blk_virt_contract.h`.
Lint: `tests/platform/lint_source_invariants.c` (`inv2:` block checks).

*Console*. `serial_virt` is a separate PD with four root-provisioned pages:
one per VMM, one for the native operator client and a separate CC frontend
page. Only the virtualizer maps all four. Root grants send-only notification capabilities for persistent wakeups
and role-bound attach endpoints. The VMM's emulated virtio-console and PL011
feed a bounded endpoint adapter; it retains bytes during backpressure and
exports them over shared sDDF byte queues. CC uses its frontend queues after
resolving the public handle and checking lifecycle authority. Input remains
queued while the guest is paused. Console bytes no longer travel through
VMM or vm_manager IPC. Only attachment and lifecycle control use IPC.

The Ubuntu bidirectional console gate requires both actual serial-PD transfer
markers and guest-echo evidence. Eight seL4 fault probes verify that neither
VMM maps the other VMM's page or CC's frontend page. The dual-guest test at
`d3da13e1` passed FreeBSD's immediate and extended suspend/resume SSH checks,
concurrent Ubuntu/FreeBSD authenticated SSH, destruction and stale-handle
rejection. Its retained image SHA-256 is
`68cf76ce00bb5ff04c60a393973c4cd241a39f0fe1d73cd7f28cc0d0656cdbd5`.
The libvmm TX backend now
retains a private descriptor snapshot and offset across full queues and retries
when the adapter frees space. It acknowledges only complete chains; traversal
and each copy are bounded. Host tests cover oversized/chained descriptors,
full-queue retry, metadata mutation, invalid indices/flags, cyclic chains and
GPA failure. At revision `8920f31e`, `make test-console-backpressure` stopped
the host drain until the backend was full with a pending descriptor, then
recovered all 262,144 position-dependent bytes exactly. Payload SHA-256:
`3d01ad5a6b80788d4152bd2e9bcd2cd86b6a340ace5d8d3f9a00e57a2b56deda`.
This qualifies that bounded sustained-output case; it does not qualify
dual-guest lifecycle behavior. MAC `task_f0be9d2f86204aa6bf06c34f6464fc0c`
retains the target and full-gate evidence.
The production available/used-ring handler also has host coverage for deferred
and exact-once completion, retained heads, cursor wrap and invalid availability.

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

The `NATIVE_RUST_TEST` image additionally includes `native_rust_probe` and
`native_rust_client`. Neither owns a device frame, IRQ or guest-execution cap.
The client receives only the test service endpoint and serial diagnostic
transport. The Rust service uses the normal `pd_entry.c` / `pd_main` entry
path and a C bridge to seL4 IPC. `make test-native-rust` checks reply payloads
from a separate C PD, including `alloc::Vec` data, alignment, exhaustion and
reuse of the Rust PD's private 64 KiB heap. The Rust service also receives only
network client 2's page, its network-only attach badge, a send-only virtualizer
notification and a receive-only capability for its own notification. Three
sequential ARP exchanges at the assigned address traverse `net_virt` and the
host NIC; each waits for notification delivery before reading the RX queue.
`make test-native-with-guest` additionally boots Ubuntu's real Casper userspace,
provisions authenticated SSH, and alternates three fresh native ARP batches
with guest pings. A test-only CC relay returns a sequence number for each new
batch; packet payloads still travel through the native client's queues.
The proof passed at `39c4f8bb` with image SHA-256
`3667077c178c9a61f9125c71ad022d108b44e0a8cb108e7f13ca7d601240b8f0`.
It qualifies this native/guest coexistence case, not a production network stack.
These test PDs are absent from the default image. Both the normal live-media
proof and the coexistence proof run nightly and on demand with retained images.
Ten `make test-native-network-isolation` images verify that reads and writes
from the native PD fault on both guest queue pages, the driver-transfer page,
NIC MMIO and driver DMA. Only the root task emits the success marker after
matching the exact fault badge, address and access direction. Every probe first
exercises the native client's authorized NIC path.
The same proof checks real async functions, executor capacity, poll budgets
and cancellation before verifying complete heap reuse. Its cooperative poll
budget does not preempt arbitrary future code; seL4 scheduling remains the
protection-domain CPU authority.

## I/O invariant

1. **One owner per device frame and IRQ.** Held today.
2. **Virtualizer is the only mux.** Shared-memory queues + notifications, not
   `MSG_NET_SEND` through IPC registers. *Held for network* (`net_virt` PD;
   enforced by the `inv2:` network lint checks and the host-backed
   `[net_virt] TX accepted by net_pd` / `[net_virt] RX delivered from net_pd`
   markers in `make test-ubuntu-virtio`) *and for block* (`blk_virt` PD;
   enforced by the `inv2:` block lint checks and the host-backed
   `[blk_virt] host media` / `[blk_virt] host-media read` markers in
   `make test-ubuntu-virtio`). Console now uses the separate `serial_virt` mux;
   its Ubuntu gate requires transfer markers from that PD (see above).
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

**Status:** museum PDs are no longer bundled or booted. The root task
spawns exactly the PDs in `src/system_desc_aarch64.c` (14 in the default
image: `nameserver`, `log_drain`, `serial_pd`, `virtio_blk`,
`block_pd`, `blk_virt`, `net_pd`, `net_virt`, `serial_virt`, `guest_vmm_primary`,
`vm_manager`, `cc_pd`, `fault_handler`, `operator_session`; `guest_vmm_secondary`, `fault_inject`,
and `test_runner` + `event_bus` are added only to the image variants that use
them), and
`agentos.toml` lists that same set and nothing else (MAC
`task_56eae59d9aa94d2d9d047f03fc9d22ad` trimmed the manifest from 39 ELFs;
MAC `task_f95d118416a24fa484c2c43f0d955b56` then dropped `controller`,
`event_bus`, `init_agent`, `agentfs`, `vfs_server`, `net_server`,
`framebuffer_pd`, and `usb_pd` from the descriptor). Museum sources are still
compiled by the root-task Makefile `IMAGES` list so they keep building, but
they are not in the image. CC-PD now calls `vm_manager` directly for dynamic
creation, status, lifecycle and console control. Its bounded handle registry
keeps public handles separate from backend slots, validates replies and
propagates start/destroy failures. `vibe_engine` is no longer a boot dependency.
The boot-guest console path (`test-guest-console`, `test-ubuntu-virtio`)
also uses the separate serial virtualizer and CC frontend queues. The
`agentOS boot complete` marker the `GUEST_OS=none` harness waits for is now
printed by `cc_pd`, the lowest-priority PD in the image, right before it enters
its request loop. `tests/platform/lint_source_invariants.c` fails if any of
the dropped PDs reappears in the default descriptor.

## QEMU host transports

On the MCS target, each VMM also holds a capability to its own guest's
scheduling context, installed by the root task using
`contracts/guest_execution_caps.h`. Suspend detaches that context from the
guest TCB; resume reattaches it. This preserves queued guest fault IPC while
removing execution budget. Failed execution transitions return an error and
retain the prior lifecycle state. This authority does not include another
VMM's guest or the driver scheduling contexts. The dual-guest qualification
above verifies resume for the configured slots. Destroy is terminal for a
slot in the current image: RAM/capability reclamation and clean guest recreation
remain work under `task_e58e8c20b539a258fc1f0ec28aeb5308`.

QEMU virtio devices are hardware stand-ins owned by canonical agentOS driver
PDs. The QEMU buses used for block media (8), networking (16), and the control
console (2) are not mapped or advertised to either guest. Linux and FreeBSD see
separate VMM-emulated virtio-net, virtio-blk, and virtio-console devices.

Reintroducing host transport DTB nodes, host IRQ registration, or direct guest
DMA against those QEMU devices is an architecture regression.

## Proof

### Framebuffer queue qualification image

`make test-framebuffer` adds `framebuffer_queue` and two native test clients
to an AArch64 image. The service owns private staging/committed surfaces and
maps two separate root-provisioned client queue pages. Each client maps only
its own page, with a send-only service notification and a receive-only local
notification; only the service can signal both clients. No component in this
variant receives a display device frame, IRQ or guest execution capability.

Root reserves the service's private surface arena as large pages before ELF
loading. Clients receive no arena mapping or frame capability. The queue
contract is `platform/include/platform/framebuffer.h`. It supports
bounded XRGB8888 surface creation, rectangular writes, committed-frame reads,
flip sequencing, status and destruction. Bulk pixels remain in shared queue
payloads, never IPC registers. Four surfaces per client and a maximum of
1024 by 768 pixels bound memory and work. Responses apply backpressure, and
both producers and consumers signal persistent wakeups when releasing work.
The host test asserts exact pixel placement, committed/staging separation,
stale-handle rejection and recovery after invalid bounds or ring occupancy.

This is a new queue service, not an extension of the retired framebuffer PD.
Its focused target test asserts real create/write/flip/status/read/destroy
transactions from both native clients. `make test-framebuffer-isolation`
boots eight images covering each client's read/write access to the other
queue page and the private arena. Each client first completes its authorized
pixel transactions; only root emits the isolation marker after matching the
fault badge, address and access direction. Both focused tests passed locally
on Spark. They do not establish hardware scanout, guest DRM/input or an
external export client. Those remain required for the v0.4 graphics outcome.

The in-progress libvmm GPU backend (`libvmm/src/virtio/gpu*.c`) implements
bounded 2D resource commands and direct control/cursor virtqueues, with
`platform/gpu-virt/framebuffer_adapter.c` translating backend operations to
the framebuffer queue contract. It is compiled into libvmm but is not yet
registered by the VMM boot path, advertised in a guest DTB, or granted a
framebuffer queue by root. Host tests verify exact pixels through the real
framebuffer queue implementation; cross-compilation is not a guest DRM proof.
This code adds no physical display ownership. Root wiring, guest proof,
input, hardware scanout and external export remain required.

### Generic PD logging

On AArch64, root provisions a separate 4 KiB log ring for each client and a
read-only configuration page containing its role and the boot identities.
Clients map only their own ring at `0x1000b000`; `log_drain` maps the client
rings at `0x2b000000`. Configuration lives at `0x1000a000`, separate from
the role-specific startup/serial-transfer address `0x10005000`.
Frame capabilities remain in root. The UART owner and drain do not log through
this client path, avoiding recursion through the diagnostic transport.

A client appends bounded bytes and signals a send-only notification capability.
It never calls the drain synchronously. Pending wakeups survive boot ordering
and a busy consumer, avoiding nameserver/drain call cycles. The drain scans
only configured slots and uses root-supplied identities, ignoring legacy
caller-selected slot and PD identifiers. Its configuration is read-only and
the old registration opcode is rejected on this path. Per-client partial lines
and bounded cursor validation keep a malformed ring from mixing another
client's output or trapping the drain in an unbounded scan.

Logs remain best effort: full rings drop new bytes, and a client controls its
own log contents. Logging is diagnostic evidence, not authorization or an
independent claim that a client is healthy. This does not establish fair CPU
service under arbitrary notification flooding.

`make test-log-rings` verifies native fragmented output through the real UART
with a root-derived identity despite invalid caller-supplied legacy IDs.
`make test-log-isolation` verifies native read/write faults on the drain's
ring region and a write fault on the read-only configuration, after successful
client logging. Root matches each fault's identity, address and direction.
Host tests additionally assert ring wrap, bounded scans, drop behavior, exact
UART bytes and interleaved partial lines. x86 remains on its reduced boot path.

`make gate-x86_64-vtx` is a separate KVM-only hardware-virtualisation
qualification. It starts one VMM PD with a VCPU bound to that PD's TCB, gives it
five EPT-mapped 4 KiB pages (a long-mode page-table walk and `HLT`), and
requires the exact HLT VM exit, guest RIP, and one-byte instruction length.
This proves only that VMX non-root entry, EPT translation, and one VM exit work
on that host. It does not qualify x86 Linux, UEFI/ACPI, guest devices, guest
I/O, persistence, lifecycle, desktop, or isolation.

### Read-only boot inspection

Root publishes one 4 KiB observation page after starting the configured PDs
and before parking. CC and the native operator client map it read-only; the
frame capabilities remain in root. `MSG_CC_INSPECT` returns the versioned packed snapshot, and
`agentctl inspect` validates it before printing structured `key=value` output.
The direct request adds no runtime root-policy loop or device authority.

The snapshot records successfully started PD identities and priorities. It
does not query live thread state: those fields remain `unknown`. Memory fields
describe the managed non-device untyped pool, its accounted-page lower bound,
and boot-reserved guest RAM. Alignment loss and sub-page kernel objects are
not included in the usage counter; subtracting it from total does not yield
free memory. Hardware fields describe the configured board and emulated guest
ABI, not proof of current device health.

`make test-inspect` checks the actual CC/CLI response and malformed-request
rejection. `make test-inspect-readonly` reads the valid page and then attempts
a write from CC; only root emits success after matching the CC fault badge,
page address, data-access kind and write direction. Neither test establishes
live scheduler inspection.

The separate `operator_session` PD is a native client, outside the TCB. It has
one serial queue page, serial-virtualizer attach/send capabilities, its own
receive-only notification and the read-only boot snapshot. It has no driver,
guest lifecycle, guest-memory or CC frontend authority. Serial contract v2
binds operator role/client 2 to its own badge, independently of the two VMM
identities and CC's frontend identity.

The line protocol accepts `inspect.snapshot` and emits a length-delimited
structured report. It retains one bounded response under backpressure,
rejects invalid lines, and drains oversized lines through their newline.
`agentctl session-inspect` uses that queue path. This remains a single,
externally serialized operator stream on the privileged CC transport; it
does not introduce independent user credentials, a PTY, an LLM or mutation.

`make test-operator-session` verifies fragmented requests, invalid-line
recovery, 128 exact reports after backpressure, and the public CLI over the
actual serial queue path. `make test-operator-isolation` runs seven images:
operator reads and writes to both guest pages and the CC frontend must fault,
as must a write to the boot snapshot. Root checks the fault identity, address
and access direction. Each image first rejects three unauthorized attach
requests and successfully attaches the operator's own channel.

`make gate` is the OS-claim gate: host suite, aarch64 and x86_64 boot with
`GUEST_OS=none`, and `gate-guest-io` (`make test-guest-net`,
`make test-guest-blk`, `make test-guest-console`). `GUEST_OS=none` alone is a
stub VMM and proves only that PDs load. `make demo-test` is the concurrent
dual-guest SSH acceptance path. The Ubuntu live-media proof is a nightly
release qualification, not a per-push gate.
