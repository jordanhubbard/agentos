# agentOS

**The world's first operating system designed for agents, not humans.**

[![License](https://img.shields.io/badge/license-BSD--2--Clause-blue.svg)](LICENSE)
[![Kernel](https://img.shields.io/badge/kernel-seL4-green.svg)](https://sel4.systems)
[![Status](https://img.shields.io/badge/status-alpha-orange.svg)]()

---

## What is agentOS?

agentOS is a real, bootable operating system built on the [seL4 microkernel](https://sel4.systems/) — the world's only formally verified, capability-secured microkernel. It's designed from the ground up for AI agents running autonomous workloads.

Every other "agent OS" is a Python framework running on Linux. They borrow a human OS, bolt some agent abstractions on top, and call it done. **agentOS is different.**

agentOS boots bare metal. Agents run in isolated address spaces with hardware-enforced capability boundaries. An agent cannot access memory, a tool, a model, or a storage namespace it doesn't hold a capability for. **The MMU enforces it. seL4 proves it.**

And here's the part that makes it unique: **agents can redesign their own environment.** The vibe-coding layer lets agents generate new system services (filesystems, message buses, tool registries), get them validated, and hot-swap them in — without rebooting. An agent that doesn't like the default filesystem can build a better one, propose it, and run it.

## Why seL4?

- **Formally verified** — mathematical proof that the implementation matches the spec
- **Capability-based security** — fine-grained, unforgeable access control
- **No heap in the kernel** — deterministic, no memory surprises
- **World-class IPC** — ~100 cycle synchronous IPC on ARM
- **Policy freedom** — kernel provides mechanisms; agents define policies

## Architecture

```
┌──────────────────────────────────────────┐
│              AGENT SPACE                 │
│   Agent A   Agent B   Agent C   ...      │
│   (seL4 isolated address spaces)         │
├──────────────────────────────────────────┤
│            SYSTEM SERVICES               │
│  CapStore  MsgBus  MemFS  ToolSvc        │
│  ModelSvc  NetStack  BlobSvc  LogSvc     │
├──────────────────────────────────────────┤
│         INIT / ROOT TASK                 │
│   (Bootstrap, resource distribution)     │
├──────────────────────────────────────────┤
│            seL4 MICROKERNEL              │
│   Capabilities · IPC · Scheduling        │
│   Memory Management (formally verified)  │
├──────────────────────────────────────────┤
│               HARDWARE                   │
│   CPU · RAM · NIC · Storage · GPU        │
└──────────────────────────────────────────┘
```

## Core Concepts

### Agent Identity
Every agent has an Ed25519 keypair. Their badge on seL4 endpoints is derived from their identity. Message senders are verified at the kernel level — unforgeable.

### Capabilities
Everything an agent can do requires a capability:
- `ToolCap` — invoke a tool
- `ModelCap` — query an LLM
- `MemCap` — read/write memory regions
- `MsgCap` — send/receive on a channel
- `StoreCap` — access storage namespaces
- `SpawnCap` — create new agents
- `NetCap` — use network resources

Capabilities are delegatable but never escalatable. An agent can grant a subset of what it holds.

### The Vibe-Coding Layer

This is the key innovation. Every system service has a defined interface. Agents can:

1. Analyze the reference implementation
2. Generate a better one (using ModelSvc)
3. Propose it as a replacement via `aos_service_propose()`
4. After validation, activate it via `aos_service_swap()`

The system is designed to evolve. The reference implementations are starting points, not permanent fixtures.

## System Services

| Service | Description |
|---------|-------------|
| `CapStore` | Capability database — tracks all cap derivations and grants |
| `MsgBus` | Inter-agent communication — channels, pub/sub, direct messaging, RPC |
| `MemFS` | Virtual filesystem — per-agent namespaces, capability-gated access |
| `ToolSvc` | Tool registry — agents register and invoke tools (MCP-compatible) |
| `ModelSvc` | Inference proxy — capability-gated LLM access, pluggable backends |
| `NetStack` | TCP/IP networking — lwIP-based, capability-gated per-endpoint |
| `BlobSvc` | Object storage — large binary objects, S3-compatible API |
| `LogSvc` | Audit logging — structured, queryable, every cap op recorded |

All services are CAmkES components. All are replaceable via the vibe layer.

## agentOS SDK (libagent)

```c
// Initialize agent
aos_init(&config);

// Send a message to another agent
aos_msg_send(dest_id, message);

// Publish to a channel
aos_msg_publish(channel, message);

// Call a tool
aos_tool_call(tool_cap, "web_search", args, args_len, &result, &result_len);

// Query a model
aos_inference_t resp = aos_model_query(model_cap, prompt, &params);

// Read/write storage
aos_store_t f = aos_store_open(cap, "/path/to/file", AOS_STORE_RDWR);
aos_store_write(f, data, len);

// Propose a service replacement (vibe-coding)
aos_service_propose("storage.v1", component_image, image_len, &proposal_id);
aos_service_swap(proposal_id);
```

## Getting Started

### Prerequisites

- macOS with Homebrew, or Ubuntu 22.04+
- 8GB RAM, 20GB disk
- QEMU for simulation (no hardware needed to start)
- **FreeBSD hosts**: cross-compile from Linux/macOS (FreeBSD LLVM cross-compilation support is limited)

### Quick start

```bash
make deps && make                             # build + QEMU (native arch) + serial output
make build TARGET_ARCH=aarch64                # ARM64 (with Linux VMM)
make build TARGET_ARCH=x86_64                 # x86_64
make build TARGET_ARCH=aarch64 GUEST_OS=freebsd  # AArch64 + FreeBSD VMM
```

### FreeBSD VMM

```bash
# Download FreeBSD 14 AArch64 disk image + EDK2 UEFI firmware
make fetch-freebsd-guest

# Build the FreeBSD VMM PD, compile DTB, pack Microkit image
make build TARGET_ARCH=aarch64 GUEST_OS=freebsd
```

### x86_64

```bash
make build TARGET_ARCH=x86_64
```

The Linux VMM is a stub on x86_64 (libvmm x86 support in progress).
Native agentOS protection domains run fully on all architectures.

### FreeBSD host

```bash
# Install build tools (LLVM, dtc, etc.)
make deps-tools

# Build with xtask gen-image — no external SDK download needed.
make build
```

### Post-boot CC-PD client

Once agentOS is running in QEMU, use `agentctl` to inspect and control guests
via the CC-PD socket:

```bash
make -C tools/agentctl

# List running guest OS instances
./tools/agentctl/agentctl --batch list-guests

# Query a specific guest
./tools/agentctl/agentctl --batch guest-status <handle>

# Snapshot a guest
./tools/agentctl/agentctl --batch snapshot <handle>
```

Run `./tools/agentctl/agentctl --help` for the full command reference.

## Project Structure

```
agentos/
├── kernel/              # seL4 kernel (submodule, Phase 1)
├── libs/
│   └── libagent/        # agentOS SDK
├── services/            # System service CAmkES components
│   ├── capstore/        # Capability database
│   ├── msgbus/          # Inter-agent messaging
│   ├── memfs/           # Virtual filesystem
│   ├── toolsvc/         # Tool registry
│   ├── modelsvc/        # Model inference proxy
│   ├── netstack/        # TCP/IP stack
│   ├── blobsvc/         # Object storage
│   └── logsvc/          # Audit logging
├── agents/
│   ├── init/            # Root/init task
│   ├── hello/           # Hello world agent
│   └── self-modify/     # Vibe-coding demo agent
├── manifests/           # Agent manifests (capabilities, resources)
├── scripts/             # Build and dev tools
├── docs/                # Documentation
├── CMakeLists.txt
└── DESIGN.md            # Full architectural design
```

## Development Status

| Component | Status | Notes |
|-----------|--------|-------|
| DESIGN.md | ✅ Complete | Full architecture documented |
| libagent API | ✅ Designed | Header complete, impl in progress |
| CapStore | 🔧 Scaffolded | Core logic done, IPC wiring pending |
| MsgBus | 🔧 Scaffolded | Core logic done, IPC wiring pending |
| MemFS | 🔧 Scaffolded | Core logic done |
| ToolSvc | 🔧 Scaffolded | Core logic done |
| ModelSvc | 🔧 Scaffolded | Core logic done, HTTP pending |
| LogSvc | 🔧 Scaffolded | Core logic done |
| Init Task | 🔧 Scaffolded | Phase structure done |
| Hello Agent | 🔧 Scaffolded | Core structure done |
| Vibe Agent | 🔧 Scaffolded | Three-phase flow done |
| seL4 CMake | 🔧 Scaffolded | Build system wired |
| QEMU boot | ⏳ Pending | Requires deps setup |
| NetStack | ⏳ Pending | lwIP integration |
| BlobSvc | ⏳ Pending | Object storage impl |
| FreeBSD VM guest | 🔧 Phase 1 | VMM PD + UEFI + multiplexer scaffolded |
| VM multiplexer | 🔧 Phase 1 | create/destroy/switch 4 VM slots |

## Philosophy

agentOS is built on a few core beliefs:

1. **Agents deserve their own OS.** Running on Linux is running on someone else's OS, designed for someone else's needs.

2. **Security is not optional.** Formal verification, capability-based isolation, and hardware-enforced boundaries are the minimum bar for a system where autonomous agents operate.

3. **Agents should design their environment.** The hardest part of building agent infrastructure is that humans are guessing at what agents need. Let agents figure it out themselves.

4. **Boot it or it doesn't count.** An "agent OS" that's a Python package is an agent library. agentOS boots.

## CUDA Compute Offload

agentOS supports GPU kernel offload via CUDA PTX embedded in WASM modules.

### How it works

1. **Embed PTX in WASM**: Add a custom section named `agentos.cuda` to any WASM module. The section payload is a raw PTX source file (must begin with `.version`).

2. **Submit via VibeEngine**: When an agent submits a WASM module with this section, VibeEngine automatically extracts and validates the PTX during `OP_VIBE_VALIDATE`.

3. **gpu_scheduler PD**: On `OP_VIBE_EXECUTE`, VibeEngine notifies the `gpu_scheduler` protection domain (priority 160, passive). The scheduler claims one of 4 static GPU slots.

4. **On Sparky GB10 (Blackwell)**: The gpu_scheduler would JIT-compile the PTX via `nvrtc` and bind it to a CUDA context on the slot. On QEMU/RISC-V (no GPU), it's bookkeeping only.

### Rust SDK

```rust
use agentos_sdk::cuda::CudaKernel;

let ptx = b".version 7.5\n.target sm_90\n.address_size 64\n\
            .visible .entry matmul(.param .u64 A, .param .u64 B, .param .u64 C) {\n\
                ret;\n}\n".to_vec();

let kernel = CudaKernel::new(ptx, "matmul".to_string());
kernel.validate()?;   // Check PTX before submitting
kernel.submit(0)?;    // Submit to GPU slot 0
// ... kernel runs on GPU ...
CudaKernel::complete(0)?; // Release slot
```

### Channel topology

```
vibe_engine (CH_GPU=2) ──notify──► gpu_scheduler (CH_VIBE=0)
controller  (CH=51)    ──ppcall──► gpu_scheduler (CH_CTRL=1)
```

---

## FreeBSD VM Guest

agentOS can run **FreeBSD 14 AArch64 as a virtual machine guest** under the seL4 hypervisor, using the [au-ts/libvmm](https://github.com/au-ts/libvmm) Microkit VMM library.

seL4 runs at **EL2** (ARM hypervisor mode) — it IS the hypervisor. No separate hypervisor layer needed.

### Boot sequence

```
seL4 (EL2)
  └─► freebsd_vmm PD (libvmm)
        └─► EDK2 UEFI firmware @ guest phys 0x00000000
              └─► bootaa64.efi → loader.efi
                    └─► FreeBSD kernel (EL1)
```

### VM Multiplexer

The `freebsd_vmm` Protection Domain is a **VM multiplexer** — it manages up to 4 independent FreeBSD instances simultaneously. The controller can create, destroy, and switch between them via IPC:

| Opcode | Operation | Args | Returns |
|--------|-----------|------|---------|
| `0x10` | `OP_VM_CREATE` | — | `slot_id` (0–3) or `0xFF` |
| `0x11` | `OP_VM_DESTROY` | `mr[0]=slot_id` | `0` ok / `1` error |
| `0x12` | `OP_VM_SWITCH` | `mr[0]=slot_id` | `0` ok / `1` error |
| `0x13` | `OP_VM_STATUS` | — | `mr[0..3]` = state per slot |
| `0x14` | `OP_VM_LIST` | — | count + `(slot_id<<8\|state)` per slot |

**Slot states:** `FREE(0)` → `BOOTING(1)` → `RUNNING(2)` ↔ `SUSPENDED(3)` → `HALTED(4)` / `ERROR(5)`

Console focus follows the active slot. When you switch, the inactive slot is suspended at the seL4 vCPU level (zero scheduling overhead).

### Quick start

```bash
# Install build deps
make deps

# Download FreeBSD 14 AArch64 disk image + EDK2 UEFI firmware
make fetch-freebsd-guest

# Build the VMM PD, compile DTB, pack Microkit image
make build TARGET_ARCH=aarch64 GUEST_OS=freebsd
```

### Creating additional VM instances

Once the first FreeBSD is running, create a second from the agentOS controller:

```c
// From controller PD: create a new FreeBSD VM
microkit_mr_set(0, 0);
microkit_msginfo reply = microkit_ppcall(CH_FREEBSD_VMM,
    microkit_msginfo_new(OP_VM_CREATE, 1));
uint8_t slot = (uint8_t)microkit_mr_get(0);   // e.g. slot = 1

// Switch console to the new VM
microkit_mr_set(0, slot);
microkit_ppcall(CH_FREEBSD_VMM, microkit_msginfo_new(OP_VM_SWITCH, 1));

// Destroy VM slot 0
microkit_mr_set(0, 0);
microkit_ppcall(CH_FREEBSD_VMM, microkit_msginfo_new(OP_VM_DESTROY, 1));
```

### Memory layout

Each VM slot gets 512MB of isolated guest RAM:

```
Guest physical address space (all slots):
  0x00000000 - 0x03FFFFFF   UEFI flash (EDK2, shared read-only)
  0x08010000                GIC CPU interface (vGIC emulation)
  0x09000000                PL011 UART (console passthrough)
  0x0a003000+               VirtIO MMIO (block device, per slot)

Per-slot RAM:
  Slot 0: 0x40000000 - 0x5FFFFFFF  (512MB)
  Slot 1: 0x60000000 - 0x7FFFFFFF  (512MB)
  Slot 2: 0x80000000 - 0x9FFFFFFF  (512MB)
  Slot 3: 0xa0000000 - 0xBFFFFFFF  (512MB)
```

### Why FreeBSD?

- BSD license aligns with seL4's formal verification story
- **Jails** map naturally to seL4 capability domains (Phase 3 roadmap)
- `pf` firewall ruleset = capability policy layer
- ZFS + GEOM: a principled storage stack for agentOS's BlobSvc
- bhyve inside FreeBSD = agents can *nest* hypervisors within agentOS

See [`docs/freebsd-vm-guest.md`](docs/freebsd-vm-guest.md) for the full design doc.

---

## Contributing

agentOS is in early development. The design is stable; the implementation is growing. Contributions welcome in:

- seL4 integration (IPC wiring, CAmkES components)
- libagent SDK implementation
- Service implementations (NetStack especially)
- Alternative service implementations (show us what a better MemFS looks like)
- Documentation and tutorials

## License

BSD 2-Clause. See [LICENSE](LICENSE).

## Acknowledgments

Built on [seL4](https://sel4.systems/) by the Trustworthy Systems group at CSIRO's Data61. The formally verified foundation that makes this possible.

---

*"Hey Rocky, watch me pull an operating system out of my hat!"*  
*"That trick never works!"*  
*"This time for sure!"*  
*[boots successfully]* 🫎
