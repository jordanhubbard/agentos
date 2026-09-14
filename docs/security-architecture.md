# agentOS architecture and security boundaries

agentOS puts device ownership, I/O multiplexing, and guest execution in
separate seL4 user-mode protection domains (PDs). Linux and FreeBSD consume
devices emulated by agentOS. Their kernels do not own the host NIC, disk, or
UART. Native agents are intended to use the same virtualizers directly.

This diagram describes the AArch64 topology after the direct CC-to-VM-manager
lifecycle change (`task_d2cfd8f55cb64e4b91e4c45ead3df6f7`). It is an implementation snapshot,
not evidence of bare-metal or x86 guest qualification. Solid arrows show
current paths; dashed arrows show bootstrap authority or planned paths.

## Current device and control paths

```mermaid
flowchart TB
  subgraph guests[Guest VSpaces — kernels treated as hostile]
    linux[Linux kernel EL1 and guest userspace EL0]
    freebsd[FreeBSD kernel EL1 and guest userspace EL0]
  end
  subgraph userspace[agentOS user-mode protection domains — EL0]
    primary[guest_vmm_primary PD<br/>vCPU and vGIC<br/>libvmm emulated virtio<br/>descriptor checks and GPA translation]
    secondary[guest_vmm_secondary PD<br/>optional second guest<br/>same VMM implementation]
    netq[Shared network region<br/>sDDF queues and notifications<br/>software-assigned client strides]
    blkq[Shared block region<br/>sDDF requests and responses<br/>software-assigned client strides]
    nv[net_virt PD<br/>network multiplexing]
    bv[blk_virt PD<br/>block multiplexing]
    nd[net_pd PD<br/>host NIC MMIO and IRQ]
    bd[virtio_blk PD<br/>host block MMIO, IRQ and DMA window]
    cc[cc_pd PD<br/>control API and console relay<br/>owns host virtio-serial transport]
    manager[vm_manager PD<br/>guest lifecycle control]
    serial[serial_pd PD<br/>owns PL011 UART]
    logs[log_drain PD<br/>serial encoder repaired<br/>generic log provisioning incomplete]
  end
  subgraph kernel[Only kernel-mode code]
    sel4[seL4 at EL2<br/>capability checks, address spaces,<br/>IPC, scheduling and vCPU mechanisms]
  end
  root[Root task — user mode<br/>allocates objects, maps frames,<br/>distributes initial capabilities, then parks]
  hardware[QEMU hardware stand-ins today<br/>NIC bus 16 — block bus 8 — control bus 2 — PL011<br/>physical boards are the target]
  operator[External agentctl / test harness]
  linux -->|emulated virtio MMIO faults| primary
  freebsd -->|emulated virtio MMIO faults| secondary
  primary --> netq
  secondary --> netq
  primary --> blkq
  secondary --> blkq
  netq --> nv
  blkq --> bv
  nv -->|RAW control calls and shared slots| nd
  bv -->|bounded chunk calls and DMA window| bd
  primary <-->|VMM-local console queues plus IPC| cc
  secondary <-->|VMM-local console queues plus IPC| cc
  operator <-->|framed control and console API| cc
  cc -->|public handle to VM slot<br/>dynamic create and lifecycle| manager
  manager --> primary
  manager --> secondary
  logs -->|serial control and shared payload| serial
  nd --> hardware
  bd --> hardware
  cc --> hardware
  serial --> hardware
  root -.->|initial capability distribution| userspace
  sel4 -.->|enforces configured authority| userspace
```

The diagram omits auxiliary nameserver/fault-handler PDs and compatibility
`block_pd` from the I/O paths. The generated system descriptor and manifest
remain the complete topology authority. A second VMM is included only in image
variants that configure it.

## How the approach differs

| Design decision | agentOS mechanism | Security consequence and limit |
| --- | --- | --- |
| A workload OS does not own the machine's devices | Guest virtio MMIO faults into a VMM; only designated driver PDs receive host device frames and IRQs | A compromised guest cannot simply program the host device. It can still attack its VMM's parser and virtual-device implementation. |
| Drivers execute outside the kernel | Driver PDs have separate VSpaces and explicit capabilities | A driver bug does not inherently gain kernel execution. Its granted device/DMA authority still matters; this is not an IOMMU isolation proof. |
| Multiplexing is a service boundary | Separate `net_virt` and `blk_virt` PDs consume bounded queues | Device access crosses a named service boundary. Shared-region mapping and resource exhaustion still require auditing. |
| A guest address is not a host pointer | VMM code validates descriptors and translates GPA to its mapped guest RAM | Invalid descriptors can be rejected before copying. Correctness of every translation and length calculation remains userspace TCB work. |
| Native work need not inherit a Linux kernel | Native PD clients are planned to attach to canonical virtualizers | The architecture can remove an entire guest kernel from a workload's dependency set. Live native virtualizer attachment is not yet qualified. |
| Control and bulk data have different contracts | seL4 IPC for attach/lifecycle; shared-memory queues for net/block payloads | Authority checks and data movement have explicit boundaries. Console still needs migration to a separate virtualizer PD. |

These choices differ from a host-kernel driver path and from assigning a host
device directly to a guest. They are not a claim that every other hypervisor
uses either design or that capability-based virtualization is unique to
agentOS.

## The compromise boundary is explicit

Guest compromise and VMM compromise are different threats. Guest kernels do
not receive the host sDDF queue mappings. Their VMMs do. At this revision the
root maps each VMM's network and block client onto separate 2 MB frames; each
VMM maps only its own client frames. net_pd maps only a separate transfer page,
which net_virt also maps. `make test-network-isolation` verifies foreign-client
and driver-page read/write faults from both VMM slots. Block
clients occupy separate 2 MB frames, and each VMM maps only its own frame.
The virtualizer alone maps all block client frames and the RAM-disk page.
`make test-block-isolation` checks read/write faults from both VMM slots at
foreign block-client and RAM-disk addresses. Each class is qualified separately.

Device selection is independently capability-bound. Root mints each VMM's
net/block endpoint with its assigned slot badge; ATTACH checks that badge
against the requested client, VMM slot, and block media before changing state
or calling a driver. `make test-virtualizer-authority` rejects spoofed
attachments from both VMM slots and then accepts their legitimate assignments.
This closes the request-based media-selection gap independently of the page
mapping boundaries.

The root task, VMMs, virtualizers, and drivers therefore remain consequential
TCB components. seL4 enforces the authority they are given; its verification
does not prove the correctness of agentOS C/Rust code, the generated capability
policy, device firmware, QEMU, or all hardware configurations. Availability,
side channels, malicious DMA, and recovery need their own evidence.

## Target topology and remaining work

```mermaid
flowchart LR
  guest[Guest virtio drivers] --> vmm[VMM PD<br/>validated emulation]
  vmm --> mux[Separate net / block / serial virtualizer PDs]
  native[Native agent PD clients] -.-> mux
  mux --> drv[Driver PDs<br/>one device class owner]
  drv --> hw[Physical device frames and IRQs]
  cc[CC-PD lifecycle API] --> manager[vm_manager]
  manager --> vmm
```

Network and block already have the separate virtualizer boundary. Console is
still a library inside each VMM. Dynamic lifecycle now calls `vm_manager`
directly; `vibe_engine` is retired from the image. Separating `serial_virt`
and attaching native clients remain implementation tasks. Physical
board execution and x86 guest execution require independent target evidence.

## Evidence and source map

| Claim or boundary | Authority |
| --- | --- |
| Booted PDs, endpoints and device assignments | [`system_desc_aarch64.c`](../kernel/agentos-root-task/src/system_desc_aarch64.c), [`agentos.toml`](../kernel/agentos-root-task/agentos.toml) |
| Shared network/block mapping rights | [`main.c`](../kernel/agentos-root-task/src/main.c), block and network mapping branches in the PD spawn path |
| Allowed device owners and remaining console gap | [`TCB.md`](TCB.md) |
| Network and block queue contracts | [`platform/include/platform/`](../platform/include/platform/), [`contracts/`](../kernel/agentos-root-task/include/contracts/) |
| Guest-visible device proofs | `make gate`: host tests, both stub-boot architectures, guest net/block/console proofs |
| Concurrent authenticated Linux/FreeBSD acceptance | `make demo-test`; remains under qualification after the retained FreeBSD SSH timeout |
| Release-level claims | [`RELEASES.md`](RELEASES.md): exact revision, gate receipt, checksums and remote verification |

The detailed diagram is an evidence-backed snapshot, not a generated
capability audit. Refresh it and the [presentation claim ledger](presentations/agentos-systems-security/FACTS.md)
when topology or qualification changes.
