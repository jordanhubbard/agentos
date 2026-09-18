# agentOS Trusted Computing Base

**This page is binding.** If a component is not listed here, it is not trusted
and must not own a device frame, an IRQ, or guest RAM. QEMU is a hardware
emulator for prototyping. On a board, the same PDs own the real devices.

This page describes two things and keeps them apart: what **boots today**
(verifiable against `kernel/agentos-root-task/agentos.toml`) and the **target
shape**. A claim that belongs to the target column is not an OS claim until the
manifest and the gate agree with it.

The optional `GUEST_INPUT` AArch64 variant adds `input_virt`, a bounded input
queue virtualizer. It owns no hardware frame, IRQ, or guest execution cap.
Root maps one event page per VMM and a separate CC frontend page; only the
virtualizer maps all three. CC resolves public guest handles before submitting
input batches. VMMs consume only their own keyboard/pointer queues and emulate
two faulting virtio-input devices. Input notifications confer send-only wakeup
authority, never access to another client's queues. Target enumeration,
delivery and mapping-isolation qualification remain pending.

## Privilege

The x86 firmware composition now allocates its VCPU and four EPT paging
objects from a dedicated 64 KiB non-device child untyped. Root moves the
sole pool capability to the owning VMM after boot configuration, in the
architecture-exclusive slot defined by `contracts/x86_guest_object_caps.h`.
The VMM's own TCB is outside this pool: x86 VMEnter executes the VCPU bound
to that thread, unlike ARM's separate guest TCB. Revocation must occur
outside VMEnter after guest I/O is quiescent. Guest RAM, ROM, their VMM
aliases and the ASID namespace remain separate resources. This establishes
allocation authority only; x86 runtime suspend/destroy, full reclamation
and reconstruction are not yet implemented. Host tests check the allocation
source, destination slots and every retype failure. Intel Debian VMX/SSH and
the full Spark gate passed at `115e1ba`; [the receipt](evidence/2026-09-18-spark/x86-private-objects.json)
records allocation/boot/I/O qualification, not runtime revocation.

The x86 firmware RAM and ROM frames now each descend from their own private
2 MiB child untyped. Root moves each sole pool cap after installing its EPT
mapping and VMM alias. ROM remains read-only in both address spaces. The
firmware VMM uses a 12-bit CNode and receives its own CNode, VSpace and EPT
root caps; none conveys a peer's memory or a host device. RAM pool slots reuse
the common guest RAM layout; the two ROM pools occupy separate slots 496/497.
`contracts/x86_guest_memory_caps.h` defines this boundary. Host tests cover
all supported reservation sizes, overflow/out-of-range indices, disjoint pool
slots and frame retype failure propagation. Target qualification of this
allocation change is pending. Runtime revocation, device quiescence and
lifecycle control remain outstanding.

Each AArch64 guest's TCB, VCPU, IPC frame and MCS scheduling context now come
from a dedicated 64 KiB non-device child untyped. After boot configuration,
root moves the pool's sole capability to that guest's VMM, alongside the
existing execution-capability slots. Revoking this pool can remove the guest
objects and root's original descendant caps without revoking the VMM's own
thread or any peer guest. Guest RAM uses separate per-frame pools below.
The execution-pool contract is `contracts/guest_execution_caps.h`; production
teardown now drains device references before revoking this pool and guest RAM.
Terminal execution/RAM revocation passed the console-proof target test at
`7f1e1d8`; [the receipt](evidence/2026-09-17-spark/guest-teardown.json)
records the exact scope and full gate. Recreation remains pending. Guest paging
and service queue grants are separate resources, not part of this pool.

ARM guest VSpaces and their intermediate page tables now come from a separate
1 MiB child untyped. Root tracks that allocation source for all later guest
mappings and refuses to fall back to its general pool if it is exhausted.
After the final guest IPC mapping, root moves the sole paging-pool cap to the
VMM. Each VMM also receives its own newly created ASID pool, never the global
ASID controller or another VMM's namespace. Teardown revokes the paging pool
after execution and RAM; the empty ASID namespace remains management authority
for future VSpaces. The contract is `contracts/guest_paging_caps.h`.
Private paging revocation and the full OS gate passed at `d00759c`;
[the receipt](evidence/2026-09-17-spark/guest-paging-teardown.json)
records the console-proof target and retained management resources.
Reconstruction remains pending.

The VMM paging helper can now retype a guest VSpace from the empty private
paging pool and assign it through the retained per-guest ASID namespace.
Guest frame mapping creates missing intermediate tables in bounded slots
3600 through 3853, with at most three table allocations per mapping and no
fallback to root or another guest's pool. The 254-table limit reserves space
for an 8 KiB VSpace on ARM configurations that require it. Paging revocation
resets the allocation cursor; a partial rebuild must be revoked before retry.
RAM rebuilding uses this mapper. Host tests cover ASID assignment, every
failure stage, retry after release, lookup-depth bounds and table exhaustion.
`make test-guest-paging-recycle` extends the queue-recycle target with two
fresh-VSpace cycles, large/small frame mapping, whole-large-page zero checks
and stale VSpace/table cap rejection after revocation. That target and the full
gate passed on Spark at `dc109dc`; [the receipt](evidence/2026-09-18-spark/guest-paging-rebuild.json)
records the two-cycle component proof. This helper does not rebuild execution objects, service mappings
or the guest image, and the production reset callback remains absent.

The execution reconstruction helper retypes a stopped TCB, VCPU, scheduling
context and IPC frame from the owning VMM's empty execution pool. It maps the
IPC frame in the fresh guest VSpace, configures the TCB and binds its VCPU,
then publishes replacement TCB/SC/VSpace caps in the private manager exchange.
The existing fault endpoint is retained. It grants no scheduling authority and
never makes a guest runnable; manager scheduling remains a separate step.
Failed reconstruction requires execution and paging revocation before retry.
Host tests cover every operation failure and recovery. The
`test-guest-execution-recycle` target extends paging recycling with two sets of
fresh objects, stopped register read/write checks and stale-cap rejection.
The target and full gate passed on Spark at `b5c14e5`;
[the receipt](evidence/2026-09-18-spark/guest-execution-rebuild.json) records
the stopped-object scope. Service reattachment, image restoration and the
production reset callback remain outstanding.

ARM `vm_manager` now configures guest scheduling between the VMM's CREATE
reply and its BOOT call. Root gives each VMM a private capability exchange
CNode containing only its guest TCB, scheduling context, VMM fault endpoint
and guest VSpace;
the manager receives both exchanges and each guest's root-selected CPU
SchedControl cap. An inert authority TCB bounds manager priority assignment
to 150. VMMs receive neither SchedControl nor that authority TCB. The manager
copies objects into private scratch slots, applies the fixed 25 ms / 100 ms
budget and priority, then deletes every temporary copy, failing closed on
lookup, configuration or cleanup errors. Root's initial configuration remains
in place. Reconstruction must publish fresh objects before replying to CREATE;
calling back into the synchronously waiting manager would deadlock. The
contract is `contracts/guest_scheduling_caps.h`. Primary managed-boot scheduling
and the full regression gate passed at `9303460`;
[the receipt](evidence/2026-09-18-spark/guest-scheduling-broker.json) records the
existing-object scope. Secondary CPU placement and reconstructed objects remain
unqualified; the VMM reset callback is still absent.
`make test-guest-scheduling` uses `GUEST_MANAGED_BOOT=1` to defer the single
guest, leaving no automatic handle-zero guest. It requires explicit manager
CREATE/BOOT, bidirectional console proof, destruction and stale-handle
rejection. Ordinary single-guest boot/teardown tests bypass manager CREATE
and cannot qualify this scheduling bridge on their own.

For the ARM GICv2 virtual CPU interface, root now moves each guest's initial
mapped frame capability to `vm_manager`. The manager is the sole runtime
mapping owner for this interface; VMMs receive no device-frame capability.
After a successful stopped-READY CREATE, it copies that guest's VSpace from
the private exchange, unmaps only that guest's retained frame cap and maps it
at fixed guest IPA `0x08010000`, then deletes its temporary VSpace copy.
Any lookup, unmap, map or cleanup error prevents BOOT. The other guest's
mapping is never unmapped or revoked. This grants no physical GIC distributor
access or IRQ-handler authority. `contracts/guest_gic_caps.h` defines the
bounded slots and address. Future reconstruction must publish a fresh
ASID-assigned VSpace with intermediate tables before CREATE returns; this
change does not implement reconstruction. Host tests cover both identities,
an initially present or absent mapping, peer preservation and each operation's
failure. The full gate and explicit manager CREATE/BOOT target passed on Spark
at `9e9862e`; [the receipt](evidence/2026-09-18-spark/guest-gic-broker.json)
records primary existing-VSpace remapping, console I/O and teardown. Freshly
reconstructed VSpaces and secondary guest mapping remain unqualified.

ARM guest network, block, serial and optional input queue frames now each
come from a private 2 MiB untyped pool. Root moves its sole pool capability
to the owning VMM. Once every service acknowledges detach, teardown revokes
these pools before execution/RAM/paging revocation, removing all descendant
frame capabilities and mappings, including root's originals. Empty pools
remain bounded reconstruction authority; driver, native, operator and frontend
frames are excluded. Graphics queues/surfaces and notifications remain separate.
`make test-guest-queue-recycle` qualifies post-destruction retyping, complete
zero checks, overwriting and stale-capability rejection for these pools.
Target qualification of this change is pending; it does not implement guest
recreation or establish peer continuity.

AArch64 guest RAM is allocated from dedicated 2 MiB child untyped pools.
Root installs the initial guest/VMM mappings, then moves each pool's sole
capability to its owning VMM. That VMM also receives its own CNode and the
two VSpaces needed to rebuild these mappings; it receives no peer VMM's
pool, CNode or VSpace. Revocation removes the guest frame descendants,
including root's initial mapping capabilities. Root remains a boot-only
allocator and does not service later reclamation requests.

The release/rebuild helpers require stopped vCPUs and drained device
references. The ARM production teardown callback stops device admission,
drains accepted block and console work, releases graphics resources, then
revokes the execution pool, RAM pools and paging pool. Failure remains non-resumable and
retryable; completed stages are not re-entered after capability revocation.
Before capability revocation it also requires a network contract-v6 detach
acknowledgment. The single-threaded virtualizer drops that client's queue
pointers, including any hub-pump entry, before replying. Subsequent wakeups
cannot access the retired queues. Root-assigned badges authorize detach in
the same way as attach; no new capability is granted. A retired client cannot
reattach without a future generation/reset contract, so old driver RX data
cannot be silently reused for a new guest. The driver vNIC remains allocated;
network detach alone does not prove its reclamation or guest recreation.
Block contract v5 likewise retires service queue pointers only after both
request and response queues are valid and empty. The virtualizer serializes
detach with its synchronous driver transfers, so an acknowledgment cannot
race an outstanding driver copy. The VMM drains accepted requests before
asking for detach; BUSY or malformed replies keep teardown retryable and
prevent capability revocation. Detach does not issue a flush or establish
durability. Media ownership and the per-client RAM
fallback disk remain allocated; retired clients cannot silently reattach.
Serial contract v4 retains terminal VMM-role detach under the existing guest
badge authority. The service stops both transfer directions, clears its guest
channel pointers and marks the frontend detached before acknowledging. It
does not wait for unread terminal bytes, which may be abandoned at destruction.
The VMM clears its local serial endpoint after acknowledgment; failed replies
keep teardown retryable before capability revocation. Peer and operator
channels retain their attachments. Serial queue pages are revoked only after
all services detach; reattachment requires an explicit generation/reset path.
Frontend admission now uses an atomic CLOSED/BUSY gate in each channel's
metadata. CC reads/writes and x86 UART steps hold BUSY while accessing queues.
Detach permanently sets CLOSED and returns BUSY until an admitted access
finishes; ending that access cannot clear CLOSED. Late accesses change no
queue cursor. The UART abandons pending local bytes after closure and does no
further UART I/O. Host tests exercise this interleaving and preserve peer and
operator progress. ARM teardown/full gate and Intel Debian VMX/SSH passed at
`6d9702f`; [the receipt](evidence/2026-09-18-spark/serial-frontend-retirement.json)
separates host interleaving coverage from target regression evidence.
This fence does not reopen channels or implement generation/reset semantics.
Input uses a one-shot detach handshake at the end of each existing VMM-owned
event page. The stopped VMM publishes a versioned request and signals the
service. Before acknowledging, `input_virt` removes that client from private
admission, clears held-key/release state and drops its page pointer. The
release-store acknowledgment is its final access to the retired page; later
frontend requests for that client are denied. This control path progresses
even when frontend response queues are full. Page ownership supplies authority
without a new endpoint or peer-page grant. The VMM waits for acknowledgment
before capability revocation, which then removes the input page. Event
wire layouts stay unchanged, and recreation still needs a generation/reset
contract rather than reusing a retired acknowledgment.
Framebuffer queues now carry an independent one-shot detach handshake.
After GPU quiescence, the VMM requests terminal retirement and waits for an
acknowledgment. The single-threaded framebuffer service clears the client's
selection and every queue/surface pointer before its final release-store to
the queue page. Detach takes priority over full or malformed request/response
rings. Observer capture and display forwarding cannot retain a source pointer
across that service iteration; completed observer snapshots remain independent
copies. ARM guest graphics queue and surface-arena frames now use thirteen
private 2 MiB pools per guest, delegated to the owning VMM. After all service
detach acknowledgments, teardown revokes these pools and their descendant
mappings before execution/RAM/paging reclamation. The observer snapshot and
physical display buffers remain separate, service-owned allocations. Empty
pools remain bounded reconstruction authority. The graphics teardown target
now requires pool retyping, complete zero checks, overwrite and stale-cap
rejection after destruction. Target qualification of this change is pending.
Queued guest faults are not serviced during teardown. Initialization rejects
lifecycle re-entry while media staging still holds guest RAM pointers.
Service grants remain owned by the VMM; recreation is not yet implemented.
`make test-guest-teardown` exercises terminal destruction
through CC after a live guest console proof, but does not prove recreation or
complete service-grant reclamation.
The provisioned Debian graphics/input guest also passed execution/RAM teardown
at `7e8ebaa`, after authenticated SSH and exact display/input checks;
[that receipt](evidence/2026-09-17-spark/seeded-graphics-teardown.json)
predates private paging revocation and does not qualify recreation.
`make test-guest-ram-recycle` exercises two preboot overwrite/revoke/retype
cycles, complete zero verification, stale capability rejection and guest
block I/O. This test is not a claim of live destroy/recreate, execution-object
reclamation, or peer continuity. The recycle test and full OS gate passed
on Spark at `3581a277c598d49dce922ebd08ea29bc59722ae5`; the
[qualification receipt](evidence/2026-09-16-spark/guest-ram-recycle.json)
records the source, image and log hashes.

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
        ├── cc_pd          owns QEMU virtio-serial (bus.2, IRQ 50): the console the
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
        ├── vm_manager     guest lifecycle and per-guest GIC virtual-interface mapping
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
maps only the fourth, driver-transfer page, and `net_virt` maps all four. Queue
allocation and mapping use the common root path on ARM and x86, with a
compile-time check that queue pages match the architecture's large-page size.
Root allocates the region only when `net_virt` is present. Allocation failure
stops startup; mapping failure prevents the affected PD from starting. Device
registers and driver DMA remain separate grants. Attachment
is one `NET_VIRT_OP_ATTACH` Call per client; after that the VMM only
signals `net_virt`'s bound notification when `tx_active` is non-empty (and
`net_virt` asked for kicks through the sDDF `consumer_signalled` flag), and
`net_virt` signals the owning VMM's bound notification when it fills
`rx_active`. Root grants send-only notification capabilities in both
directions; pending wakeups survive until received. Guest wake badges may
combine with the native-client badge and are handled before IPC tags.
`net_virt` alone speaks `net_pd`'s RAW contract (`RAW_SEND` / `RAW_RECV`
over a per-client slot in the same frame); `net_pd` NBSends `RX_READY` to
`net_virt`, never to a VMM, and no VMM holds a `net_pd` endpoint. When
`net_pd` reports no host NIC, `net_virt` wires the clients into the
sDDF-shaped hub/loopback pump instead. Contract:
`include/contracts/net_virt_contract.h`. Lint: `tests/platform/lint_source_invariants.c`
(`inv2:` network checks).

The VMM network adapter accepts an explicit emulated MMIO base, interrupt and
mapped queue-region base. The ARM entrypoint keeps its existing defaults.
Initialization refuses invalid alignment or a second binding before touching
queues, and a failed virtualizer attachment does not register a guest device.
The x86 host test exercises exact packet TX/RX through the real libvmm network
backend, the canonical loopback pump and IOAPIC assertion/acknowledgment.
Both architecture SDK builds compile the adapter; these checks do not prove an
Intel host NIC, root network grants or a Linux x86 network interface.

The NIC driver uses the shared host virtio transport for feature negotiation,
status, configuration and independent RX/TX queue handles. Root DMA metadata
version 1 selects ARM MMIO; version 2 supplies bounded modern PCI spans.
MMIO interrupt status and acknowledgment also go through that transport.
PCI bindings have no ISR mapping: the driver polls RX and nonblocking IPC,
with root limiting it to 1 ms of CPU per 10 ms period. The driver
requires offered MAC and VERSION_1 features and waits for reset completion
before configuring queues. The Intel composition includes `net_pd` and
`net_virt`; the VMM attaches through the same contract and refuses to continue
unless the virtualizer reports an initialized host NIC. A loopback attachment
does not meet that condition. Pending network wakes share the VMM notification
path with block and serial. This attachment proves driver initialization, not
guest packet I/O or a Linux network interface.

The generated guest DSDT advertises console, block and network as separate
LNRO0005 devices with integer UIDs 0/1/2, MMIO pages at 0xf0000000/1000/2000
and GSIs 16/17/18. Hardware attachments advertise the MAC returned by
`net_virt`, matching the NIC driver's client address; loopback retains the
platform default MAC. The Intel userspace qualification opens Linux `eth0`,
checks its MAC, raises the interface and performs a packet-socket ARP exchange
with the local QEMU gateway. It checks all 42 protocol bytes in the reply and
bounds the wait to 20 seconds. VMM completion additionally requires host
attachment, guest DRIVER_OK and observed TX consumption and RX activity.
This qualifies Ethernet packet I/O, not IP configuration, DHCP, SSH, Internet
connectivity or receive-latency guarantees.

The Intel firmware qualification machine assigns modern virtio block to PCI
00:05.0 and network to 00:06.0. Root discovers both through the same bounded
configuration-port path: validate the device identity, disable decoding and
bus mastering while sizing BARs, verify restoration, then delete the temporary
port capability. Root allocates pages in ascending physical order across both
devices and rejects any page shared between device classes. Only `net_pd`
receives uncached NIC registers and the private DMA frame; PCI bus mastering
is enabled after both mappings succeed. The virtualizer and VMM receive only
their existing queue grants. Allocation or mapping failures prevent startup.
The qualification NIC
uses a restricted user network with no forwarded host ports.

`net_virt` emits complete bounded diagnostic messages through the common log
ring and send-only drain notification. It holds no serial service endpoint,
serial transfer page or log-drain Call endpoint. Nameserver, NIC-driver and
VMM notification capabilities remain separate from logging. Release builds
without log provisioning retain the common silent debug fallback.

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
neither VMM maps it or the other client's page. The common root path allocates
these pages only when `blk_virt` is present and checks the architecture's
large-page size against the layout. Allocation failure aborts startup;
mapping failure prevents the affected PD from starting. Device and DMA grants
remain separate. Control is one `BLK_VIRT_OP_ATTACH` Call per
client, during which `blk_virt` probes the media and fills the client's sDDF
`storage_info`; after that the VMM signals the virtualizer's bound notification
when its request queue is non-empty (and `blk_virt` asked for kicks through
the `req_consumer_signalled` word), and `blk_virt` signals the owning VMM's
bound notification when it queued responses. Both directions use send-only
capabilities and retain pending wakeups until received. Preboot media staging
therefore does not depend on a later guest exit to retry a dropped event.
Receivers classify notification badges before interpreting IPC message tags.
The VMM block backend has a one-way, nonblocking drain mode for lifecycle
cleanup. It stops admitting new guest descriptor chains while finishing
accepted chunks and read-modify-write requests against still-mapped guest
RAM. Failed predecessors also release waiting requests; a waiter starts only
when no active writer overlaps its transfer window. Drain completion requires
empty request/response queues and no allocated request bookkeeping. Completion
interrupts are suppressed during drain, and the VMM disables its response
callbacks when the drain succeeds. This API is not yet connected to live RAM
reclamation. Host tests execute the production backend; the dedicated
`make test-guest-block-drain` target stops admission with a real pending guest
block response and requires the drain and guest block completion markers.
`blk_virt` alone holds
the `virtio_blk` endpoint and alone (besides the driver) maps the driver's
bounded DMA window, through which it chunks each request by Call; the VMM
maps no DMA window and holds no `virtio_blk` endpoint, so two guests cannot
race in that window. A profile that stages its initrd from media
(`INITRD_FROM_MEDIA`) has the VMM act as its own queue client before the
guest runs. When `virtio_blk` reports no media, `blk_virt` serves a
per-client RAM disk instead. Contract: `include/contracts/blk_virt_contract.h`.
Lint: `tests/platform/lint_source_invariants.c` (`inv2:` block checks).

`blk_virt` emits bounded diagnostic messages through the common log ring and
send-only drain notification. It has no UART endpoint, serial diagnostic
transfer page or log-drain Call endpoint. Nameserver registration and block
driver Calls remain separate from logging. Builds without provisioned log
rings retain the common debug fallback, which is silent in release builds;
linking the x86 virtualizer alone does not prove Intel block I/O.

The host block driver's register access goes through `virtio_host_transport`.
Its live ARM binding uses virtio-MMIO v2. The modern PCI binding accepts only
already mapped common, device-config and notification spans; PCI discovery,
resource grants and bus-master enablement remain a separate boot integration
step. It uses the specified byte/halfword/dword register widths, validates
queue capacity and notification offsets before enabling the queue, and reads
64-bit capacity with a bounded configuration-generation retry. The driver
still owns all DMA and uses the same request chain for either transport.
The boot-read API refuses RAM fallback, insufficient output space and reads
after guest DRIVER_OK. It submits through the existing sDDF queue and copies
data only after a matching successful response. A binding is not a device
reset or DMA revocation operation.

The shared host transport also supports independent queue handles for drivers
with separate RX and TX queues. Each handle retains its owner, queue index and
PCI notification offset; configuring another queue does not overwrite it.
A status reset invalidates existing handles. Drivers must serialize register
access and discard handles when rebinding a transport. The existing block
driver keeps its single-queue interface. Multi-queue host register tests are
not evidence of a live PCI NIC or DMA teardown.

The x86 firmware qualification board places a modern virtio block device at
PCI 00:05.0 with a fresh, read-only test medium. Root performs boot-time
configuration discovery using a temporary CF8/CFC port capability. It disables
decode and bus mastering while measuring BARs, validates the common, notify
and device capability spans, verifies BAR/command restoration, then deletes
that port capability before starting PDs. Failed discovery refuses startup.
Only `virtio_blk` receives uncached mappings of the PCI register pages. The
driver and `blk_virt` share one coherent DMA frame; the VMM maps only its own
block queue page. Root enables memory decoding and bus mastering, with INTx
disabled, only after the driver mappings succeed. The canonical driver resets
the device before configuring its polling split queue. Metadata version 2
describes PCI spans; version 1 retains the ARM MMIO layout.

The Intel firmware qualification first reads one 4096-byte block through the
VMM queue, `blk_virt` and the PCI driver, requiring the test-medium prefix and
zero padding exactly. Generated ACPI advertises the VMM's emulated block
device separately from its console. The userspace qualification additionally
requires Linux to open `/dev/vda`, check its 32 MiB capacity and read a distinct
4096-byte block at offset 4096, verifying its prefix and zero padding. The VMM
requires a completed guest queue request after DRIVER_OK before accepting the
userspace result. These read-only checks do not establish writable persistence
or pinned Debian acceptance. Root passes a zero nameserver startup
argument to PDs without that endpoint, so the standalone mux does not attempt
registration through an ungranted capability.

`make gate-x86_64-storage` separately creates a disposable 32 MiB disk and
boots two hash-checked initramfs variants. The first checks a pristine block,
writes 4096 bytes and requires Linux `fsync` to succeed. After that platform
instance exits, the host verifies the entire disk against the expected image.
A fresh platform boot opens the same disk read-only and verifies the persisted
block; the host checks the entire image again. The harness retains the disk,
both root-task/image pairs and a receipt with their hashes. This qualifies a
single guest across complete platform cold boots; concurrent-client isolation,
guest lifecycle reset and pinned Debian acceptance remain separate requirements.

*Console*. `serial_virt` is a separate PD with four root-provisioned pages:
one per VMM, one for the native operator client and a separate CC frontend
page. Only the virtualizer maps all four. Root grants send-only notification capabilities for persistent wakeups
and role-bound attach endpoints. The VMM's emulated virtio-console and PL011
feed a bounded endpoint adapter; it retains bytes during backpressure and
exports them over shared sDDF byte queues. CC uses its frontend queues after
resolving the public handle and checking lifecycle authority. Input remains
queued while the guest is paused. Console bytes no longer travel through
VMM or vm_manager IPC. Only attachment and lifecycle control use IPC.

`serial_virt` emits diagnostics through its root-provisioned log ring and
send-only drain notification. It has no UART service endpoint or serial
diagnostic transfer-page mapping, and no nameserver or log-drain Call endpoint.
Its queue pump therefore never waits for a diagnostic service reply. On builds
without log rings, the common logger retains its existing debug-output fallback;
the x86 firmware composition uses that fallback rather than a UART Call endpoint.

The shared virtio GPA layer requires an explicitly installed guest RAM
translator. Before binding, translation, nonempty payload copies and queue
activation fail; there is no identity-address fallback. Clearing the hook
disables subsequent translations but does not revoke cached ring aliases.
Teardown must separately quiesce devices, reset rings and revoke RAM authority.
The [GPA binding receipt](evidence/2026-09-17-spark/virtio-gpa-required.json)
records host rejection checks and the full Spark gate; it does not establish
Intel device integration or target-side teardown qualification.

The Ubuntu bidirectional console gate requires both actual serial-PD transfer
markers and guest-echo evidence. Eight seL4 fault probes verify that neither
VMM maps the other VMM's page or CC's frontend page. The dual-guest test at
`d3da13e1` passed FreeBSD's immediate and extended suspend/resume SSH checks,
concurrent Ubuntu/FreeBSD authenticated SSH, destruction and stale-handle
rejection. Its retained image SHA-256 is
`68cf76ce00bb5ff04c60a393973c4cd241a39f0fe1d73cd7f28cc0d0656cdbd5`.
The same scenario passed again on Spark at clean revision `2fd71cde`, with
image SHA-256 `d27a2a106c210b230dc47e2e9a6de3441e5b43828443431f1a2599f30c68f96b`.
[The qualification record](evidence/2026-09-16-spark/dual-guest.json) identifies
the retained image, serial log, network capture and test log. It does not
qualify RAM/capability reclamation, guest recreation or concurrent Debian.
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

The optional AArch64 `DISPLAY_RAMFB=1` composition adds `display_ramfb` as
a display driver. It alone receives QEMU fw_cfg MMIO, its private uncached
DMA allocation, and two private contiguous scanout banks. Its only client is
`framebuffer_queue`, through a separate queue and dedicated notifications.
No guest VMM receives these frames or caps. The framebuffer service forwards
only client zero's committed rectangle. `make test-display` verifies every
pixel of its native test frame in a QEMU display capture. `make test-guest-display`
also checks every pixel of a 1024x768 Linux guest frame while the guest is
suspended, then verifies resumed SSH and input delivery. This supplies no
bare-metal Spark GPU support or peer guest display-isolation proof.

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
boots sixteen images covering each client's read/write access to the other
queue page, private surface arena, observer page and private snapshot arena.
Each client first completes its authorized
pixel transactions; only root emits the isolation marker after matching the
fault badge, address and access direction. All sixteen cases passed on Spark
at `3b00d93`, including exact observer exports from both native clients in
each image. [The qualification record](evidence/2026-09-16-spark/framebuffer-observer.json)
identifies all sixteen retained images. Hardware scanout and guest DRM/input
remain required for v0.4.

The in-progress libvmm GPU backend (`libvmm/src/virtio/gpu*.c`) implements
bounded 2D resource commands and direct control/cursor virtqueues, with
`platform/gpu-virt/framebuffer_adapter.c` translating backend operations to
the framebuffer queue contract. The AArch64 `GUEST_GRAPHICS=1` variant adds
`framebuffer_queue` and grants each VMM only its own client page and dedicated
read/send notification capabilities. These notifications are separate from the
VMM's bound network/block/console notification. Only the framebuffer service
maps private surface storage and both client pages. The guest profile's GPU
flag selects VMM initialization and the faulting DTB window at `0x0a040000`,
virtual INTID 54. No physical GPU frame or IRQ is granted to either VMM or the
framebuffer service. The `debian-gpu` profile exercises this variant.
Host tests verify exact pixels through the real framebuffer queue implementation;
the framebuffer service receives a 1 ms budget per 10 ms period and a 1 KiB
scheduling context with additional refill records. Short queue exchanges must
not discard most of the available budget through refill coalescing. The kernel
retains a 10% CPU ceiling. CC retains its existing 1% ceiling with a 100 us
budget per 10 ms period and a 1 KiB scheduling context with extra refill
records. Its host VirtIO polling yields must not defer each request or reply
for the old one-second period. Other PD and guest scheduling parameters are
unchanged. The shorter period needs target latency and integration qualification;
it does not grant CC any additional device or guest-memory authority.
Combined guest graphics/input qualification at this scheduling revision is
pending. A physical display driver and target peer-input isolation also remain
required; native observer exports do not establish either property.

The graphics and focused framebuffer variants also grant CC a separate observer queue.
Only CC and `framebuffer_queue` map that page; neither VMM receives it.
The framebuffer service alone maps the additional private snapshot arena.
CC resolves public guest handles before requesting a capture. The observer
can capture, read and release immutable copies of selected committed frames;
it cannot modify surfaces. Snapshot cookies are scoped to the existing
privileged, serialized CC transport, not a new public authentication boundary.
The focused target image exported exact 40 by 40 frames from both native
clients through CC in multiple chunks on Spark. Guest capture remains pending.
The focused framebuffer image uses two explicit test-only public handles for
the native pixel producers; those handles are absent from production images.

The retired `services/legacy-pds/framebuffer_pd.c` rejects `HW_DIRECT`
creation with `FB_ERR_BAD_BACKEND`. Its former MMIO probe and successful
no-op flips did not implement GPU queues, resources or scanout, and have
been removed. It owns no display device and supplies no Spark GPU support.
The host integration suite invokes its actual IPC handler to verify this
rejection and that failed hardware requests consume no surface slots.

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

`make gate-x86_64-firmware-modes SEL4_SDK_VERSION=2.3.0` uses the same VCPU
and five EPT pages to qualify real-address, unpaged protected and long-mode
entry. It checks the relevant control readback and exact HLT exit in each
mode before reporting a distinct aggregate result. It does not load firmware,
execute a reset vector or prove guest-driven mode transitions. Device and
guest-memory authority remain the same as the narrow VTX proof above.

The optional [OVMF reset variant](x86-firmware.md) replaces the five-page
payload with 32 MiB private RAM and 4 MiB read-only firmware. Root initializes
these frames through temporary mappings, installs EPT mappings and provides
the single VMM's VCPU cap, writable private RAM and read-only ROM aliases for
bounded page-table walks, MMIO decoding and firmware input writes. The REP
input handler validates a bounded chunk before modifying RAM or fw_cfg state
and commits page-table accessed/dirty bits. No device frames,
IRQs or host I/O capabilities are added. The VMM emulates private PCI
configuration, scalar firmware-data ports, and bootstrap xAPIC timer state.
Bounded reads in the declared absent TPM aperture return all ones; writes
are rejected. Validated firmware ROM stores are ignored without granting
write authority or changing ROM bytes. Live APIC divider changes preserve
the private countdown; no host APIC access or timer IRQ authority is added.
The firmware composition also starts the canonical `serial_virt` PD. Root
allocates its four isolated queue pages, maps only client zero into the VMM,
and supplies a role-bound attach endpoint and send-only wake capabilities.
The VMM must attach successfully and observe empty fresh queues before boot
continues. The userspace qualification also requires foreign-client and
frontend attachment attempts to be rejected. The VMM registers the real libvmm
console at guest MMIO address `0xf0000000`, GSI 16, and pumps its bounded
serial endpoint after faults and serial-service notifications. Notification
re-entry restores seL4's three saved input words without rewriting guest GPRs;
pending console interrupts are routed on the next bounded VMX timer exit.
The userspace qualification requires a wake from the actual serial service
capability before accepting the PID 1 result. Unknown notifications remain
fatal. The generated DSDT describes this guest-only console through `LNRO0005`,
with a coherent 4 KiB read/write MMIO resource and a level/high GSI 16.
The userspace probe mounts devtmpfs and opens `hvc0`; the VMM also requires
negotiated console readiness. The x86 composition starts `serial_pd` as the
COM2 frontend: root grants only I/O ports `0x2f8..0x2ff`, the isolated frontend
queue page and its role-bound serial attach/send capabilities. COM1 remains
root's bootstrap diagnostic port. The driver disables UART interrupts and
polls at most 64 bytes per direction per yield, retaining bytes under
backpressure. Its scheduling context permits 1 ms of work per 10 ms period;
the default one-second PD refill period is unsuitable for UART polling.
It owns no guest RAM, other I/O ports or device frames. The serial virtualizer
remains the only component mapping both frontend and VMM queue pages.
The Intel userspace gate exchanges exact request/reply bytes over QEMU COM2
and the guest's raw `hvc0` before accepting its completion trap. This does not
qualify sustained UART throughput, lifecycle support or the external CC API.
The opt-in [EFI payload variant](x86-boot-payload.md) provisions 256 MiB
private RAM and embeds SHA-256-pinned kernel/initrd/command-line blobs in the
VMM's read-only ELF sections. These sources are not mapped into guest EPT;
the guest receives bytes only through checked fw_cfg reads into its private
RAM. Descriptors bind once before reads and each transfer validates at most
1 KiB before committing. This is boot input, not a block-device backend or
persistent storage. Allocation failure aborts the qualification image; runtime
resource-profile admission and teardown reclamation remain unqualified.
The bootstrap CPU admits SYSCALL support and validates EFER.SCE/LME/NXE.
Its four syscall MSRs use seL4's existing per-VCPU storage and context switch
API; no host MSR capability or passthrough is added. Actual guest userspace
syscalls and concurrent syscall-state isolation still require target proof.
The optional firmware VMM now enables VMX preemption-timer exits through its
existing VCPU cap and reads the timer rate through the kernel's restricted MSR
interface. It maintains private IRR/ISR state, checks CPU interruptibility,
injects eligible timer vectors and handles guest EOI. No new hardware authority
is granted. The [timer receipt](evidence/2026-09-17-spark/ovmf-timer.json)
records partial OVMF delivery evidence; halt/window qualification remains open.
The firmware VMM also owns a [private RTC calendar](x86-rtc.md), advanced
from its admitted clock with an explicit virtual boot epoch. It emulates
calendar, alarm and polled status state without a host RTC frame, port or IRQ.
It grants no persistent-time or host wall-clock authority; RTC IRQ enables
remain rejected. The [RTC receipt](evidence/2026-09-17-spark/ovmf-rtc.json)
records continuation to the next unsupported ACPI PM control access.
The subsequent [PM1 model](x86-pm.md) retains private mode/control and polled
timer status, without additional caps or hardware authority. SCI enables,
SMI and sleep requests are rejected; this is not a power-management lifecycle
implementation or a proof of guest ACPI table installation.
There is no legacy PIC or ISA interrupt source. Its absent command/mask
ports return all ones and discard byte writes without mutable IRQ state.
Other widths and unknown ports remain rejected. CPUID now describes the
admitted invariant clock and private local xAPIC. Fixed edge IPIs can set
pending bits only in the sole provisioned vCPU's private controller; no
host APIC access, AP creation or additional capability is involved.
Linux kernel entry is observed, but userspace, dedicated guest interrupt
handler proofs, runtime resource management and persistent firmware variables
remain unqualified.

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
guest lifecycle, guest-memory or CC frontend authority. Serial contract v4
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
