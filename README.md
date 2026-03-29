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

### Setup

```bash
git clone https://github.com/jordanhubbard/agentos
cd agentos
./scripts/setup-dev.sh
```

### Build and Run (QEMU)

```bash
mkdir build && cd build
cmake -G Ninja -C ../settings.cmake ..
ninja
ninja simulate
```

You should see the agentOS banner, init task startup, system services coming online, and the hello-agent announcing itself on `system.broadcast`.

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
