# agentOS — Design Document

**Version:** 0.1.0-draft
**Author:** Jordan Hubbard
**Date:** 2026-03-28
**Status:** DESIGN PHASE

> **Read this first — design vs. proof.** This document describes the *intended*
> architecture. Much of it is aspirational. The implementation status of each
> subsystem is tracked by **proof level** in the table below; do not read prose
> in this document as a claim that something is finished on a booted target.

---

## 0. Implementation Status (Proof Levels)

agentOS is **alpha**. Per the project proof policy (`PLAN.md`), host-only mocks
may exist but **cannot** be cited as proof of production IPC or bare-metal
behavior. Each subsystem below is labeled by how strongly it is actually proven,
not by whether code exists. When a level is uncertain, the more conservative
label is chosen.

| Level | Meaning |
|-------|---------|
| **boot-proven** | Run on a booted seL4 target under QEMU (or hardware) and asserted by an automated E2E/boot test. |
| **target-tested** | Built into the seL4 target image and validated against the target, short of full end-to-end boot of the feature. |
| **host-tested** | Validated only by host-compiled tests (`-DAGENTOS_TEST_HOST`) with seL4/Microkit IPC stubbed. Proves contract/logic shape, not IPC or hardware. |
| **stubbed** | Links and returns a defined value, but the behavior is a placeholder / `not implemented` / no-op. |
| **planned** | Described here; little or no implementation. |

| Subsystem | Proof level | Evidence / notes |
|-----------|-------------|------------------|
| seL4/Microkit boot (AArch64, x86_64) | boot-proven | `xtask qemu-test`, `tests/end_to_end_boot_test.sh` wait for boot markers; RISC-V builds but is not regularly boot-asserted |
| Linux/Ubuntu guest boot | boot-proven | `tests/e2e/run_dual_os_e2e.sh` boots + SSHes in; `make test-guest-login` |
| Emulated virtio-net (sDDF pump, IPA `0x0A010000`) | host-tested | `aos_net_virt_pump`; linux_vmm calls `virtio_mmio_net_init`; QEMU passthrough is still the SSH path |
| FreeBSD 15.0 guest boot | boot-proven | Dual-OS E2E (`3f5365a` "prove dual linux freebsd lifecycle") |
| Guest VMM slot mux (create/switch/list/status) | target-tested | Per-guest VMM slots (`470679f`); contract exercised |
| Guest snapshot / restore | stubbed | `vm_manager.c`: "SNAPSHOT/RESTORE: not implemented (Phase 1)"; `cc_pd.c` returns `CC_ERR_RELAY_FAULT` |
| Guest live-migrate (`MSG_VIBEOS_MIGRATE`) | planned | Contract defined; no target-validated implementation |
| Guest virtual IRQ injection | stubbed | `vm_manager.c` `vmm_inject_irq` logs `(stub)` |
| Dynamic guest CREATE/LIST/DESTROY via vibe_engine | host-tested | AArch64 links `vmm_mux_stub.c`; vibe_engine surfaces "phantom" `RUNNING` guests (`e70d955`) — lifecycle UX works against stubbed VM backing only |
| serial-mux / serial PD | boot-proven | Guest console login over the serial path |
| net-service / net_isolator | host-tested | `tests/contracts/net_*`; not boot-asserted |
| block-service / block PD | host-tested | `tests/contracts/block_*`; VirtIO-blk path not independently boot-asserted |
| usb-service | stubbed | `usb_pd.c` "stub mode" unless built with `AGENTOS_USB_PD` and real MMIO |
| timer-service | host-tested | `tests/contracts/timer_test.c` |
| entropy-service | host-tested | `services/entropy-service/`; not target-validated |
| CC-PD host API (list/status/console) | boot-proven | `build/cc_pd.sock`; proven by guest-login E2E |
| CC-PD snapshot relay | stubbed | Returns `CC_ERR_RELAY_FAULT` for the boot guest |
| VibeOS lifecycle API (`VOS_*`) | host-tested | `make test-vibeos-contract`, `tests/api/test_vibeos*.c` (`-DAGENTOS_TEST_HOST`) |
| vibe-engine WASM hot-swap | host-tested | `tests/integration/vibe_hotswap_test.c` exercises read/probe paths only |
| Tracing (trace_recorder PD) | host-tested | 512-entry ring (START/STOP/QUERY/DUMP); host contract tests |
| Agent-facing services (CapStore, MsgBus, MemFS, ToolSvc, ModelSvc, NetStack, BlobSvc, LogSvc) | host-tested | Contracts in `contracts/`; host tests; not boot-proven as live PDs |
| Vibe-coded service swap (`aos_service_propose`/`aos_service_swap`) | host-tested → planned | Read/probe proven on host; real propose+validate+swap not proven on target |

---

## 1. Vision

agentOS is the world's first real operating system built *for* agents, not humans. It's not a Python framework with "OS" in the name. It's a bootable, capability-secured, formally-verifiable microkernel OS where:

- **Agents are first-class citizens** — not processes pretending to be agents
- **Everything is a capability** — memory, IPC, tools, models, storage, comms
- **Agents design their own environment** — pluggable filesystems, object stores, communication protocols, all vibe-coded by agents to suit their own needs
- **The kernel trusts no one** — formally verified seL4 microkernel at the base, capability-based isolation between all components
- **No human UI required** — the "desktop" is a capability graph, not pixels

### Why seL4?

seL4 is the only microkernel with:
- **Formal verification** — mathematical proof that the implementation matches the spec
- **Capability-based security** — fine-grained access control at the hardware level
- **No dynamic kernel allocation** — deterministic, no heap, no surprises
- **World-class IPC performance** — ~100 cycle IPC on ARM, critical for agent-to-agent communication
- **Policy freedom** — the kernel provides mechanisms, not policies. Perfect for agents who want to define their own policies.

### Why Not Just Another Framework?

Existing "agent OSes" (AIOS, OctopusOS, etc.) are Python libraries running on Linux. They:
- Share a monolithic kernel with everything else
- Have no real isolation between agents
- Can't guarantee capability boundaries
- Don't control the hardware
- Are "OS" only in name

agentOS boots bare metal. Agents run in isolated address spaces with capability-mediated access to everything. An agent can't access a tool, model, or memory region it doesn't have a capability for. Period.

---

## 2. Architecture Overview

Binding TCB: `docs/TCB.md`. Binding plan: `PLAN.md`.

QEMU (or a physical SoC) is **hardware**. agentOS owns that hardware in
user-mode driver PDs, muxes it in virtualizer PDs, and presents **emulated
virtio** to Linux and FreeBSD. Native agents attach to the same virtualizers.

```
 Hardware / QEMU stand-in
        │
        ▼
 seL4 (EL2) ──────────────────────────────────────────────
        │
        ├── root task (untyped, spawn, caps; no policy)
        │
        ├── driver PDs     (UART, NIC, disk)     EL0
        ├── virtualizers   (sDDF queues)         EL0
        └── VMM            (vCPU, vGIC, virtio device emu) EL0
                │
                ├── Linux guest     EL1  (in-tree virtio)
                └── FreeBSD guest   EL1  (in-tree virtio)

 Native agent PDs ──queues──► virtualizers   (no guest OS required)
```

CapStore / MsgBus / ModelSvc are **optional clients**, not the OS. See the
museum list in `docs/TCB.md`.

---

## 3. Core Concepts

### 3.1 Agent Identity (AgentID)

Every agent has a cryptographic identity:
- **AgentID**: Ed25519 keypair, generated at agent creation
- **Capability badge**: seL4 badge on endpoint capabilities, tied to AgentID
- **Trust level**: Assigned by the init task or a trust authority agent
- All IPC messages carry the sender's badge — unforgeable by seL4 guarantee

### 3.2 Capability Model

seL4's capability system is extended with agentOS semantics:

| Capability Type | Description |
|----------------|-------------|
| `ToolCap` | Right to invoke a specific tool/function |
| `ModelCap` | Right to query a specific LLM/model endpoint |
| `MemCap` | Right to read/write a memory region (shared or private) |
| `MsgCap` | Right to send/receive on a message channel |
| `StoreCap` | Right to access a storage namespace |
| `SpawnCap` | Right to create new agents |
| `NetCap` | Right to use network resources (with bandwidth/endpoint constraints) |
| `SelfCap` | Right to introspect/modify own state |

Capabilities are **delegatable** — an agent can grant a subset of its capabilities to another agent, but never escalate beyond what it holds.

### 3.3 The Agent Lifecycle

```
CREATE → PROVISION → BOOT → RUN → SUSPEND/RESUME → TERMINATE
  │          │         │       │         │               │
  │    Assign caps  Load code  │    Save/restore      Revoke
  │    + memory    + config    │    agent state       all caps
  │                            │
  │                       ┌────┴────┐
  │                       │ RUNNING │
  │                       │         │
  │                       │ - IPC   │
  │                       │ - Tools │
  │                       │ - Model │
  │                       │ - Store │
  │                       └─────────┘
```

### 3.4 Message Bus (AgentIPC)

Built on seL4's IPC primitives but with agent-native semantics:

- **Synchronous call/reply** — for tool invocations, model queries
- **Asynchronous notifications** — for events, signals, heartbeats
- **Shared memory channels** — for bulk data (model weights, large context windows)
- **Multicast groups** — for agent coordination (built on notification objects)

Message format:
```c
typedef struct agent_msg {
    agent_id_t sender;          // Verified by kernel badge
    uint32_t   msg_type;        // text, blob, capability, event, rpc
    uint32_t   schema_version;  // Protocol version
    uint32_t   payload_len;
    uint8_t    payload[];       // Structured (Cap'n Proto or similar)
} agent_msg_t;
```

### 3.5 Pluggable Services (The "Vibe-Code" Layer)

This is the key design idea: **agents design and hot-swap their own system
services**. (Proof level: **host-tested** — the hot-swap pipeline's read/probe
paths are validated; a real propose+validate+swap on a booted target is not yet
proven. See Section 0.)

Each system service is a CAmkES component with a well-defined interface. The agentOS SDK provides:

1. **Service Interface Definitions (SIDs)** — like `.camkes` interface files but agent-oriented
2. **Reference implementations** — basic FS, basic message bus, basic tool registry
3. **Hot-swap protocol** — an agent can propose a replacement component, the trust authority validates it, and the init task swaps it in without rebooting

Example: An agent decides the default flat filesystem sucks for its retrieval patterns. It vibe-codes a vector-indexed content-addressable store, packages it as a CAmkES component implementing the `StorageService` SID, and requests installation. If it passes validation (memory bounds, capability safety, no kernel violations), it gets swapped in — for that agent's namespace only, or system-wide if approved.

**Pluggable service categories:**
- **Storage** — filesystems, object stores, vector DBs, content-addressable stores
- **Communication** — message formats, routing, pub/sub, consensus protocols
- **Scheduling** — agent priority, resource allocation, deadline management
- **Tools** — tool registries, discovery, composition, caching
- **Models** — inference routing, model selection, context management, quantization
- **Memory** — working memory, episodic memory, shared knowledge bases

---

## 4. System Services (Reference Implementations)

> Proof level for this entire section: **host-tested** unless Section 0 says
> otherwise. These describe intended reference behavior; the implementations are
> validated by host-compiled contract tests, not proven as live PDs on a booted
> target.

### 4.1 CapStore — Capability Database

- Tracks all capability derivations
- Supports queries: "which agents can access tool X?", "what can agent Y do?"
- Built on seL4's Capability Derivation Tree (CDT)
- Provides `grant`, `revoke`, `query`, `audit` operations

### 4.2 MsgBus — Agent Communication Hub

- Routes messages between agents
- Supports named channels (like SquirrelBus topics)
- Handles presence/heartbeat
- Queue-backed for async delivery
- Built on seL4 endpoints + notification objects + shared memory

### 4.3 MemFS — Virtual Filesystem

- Agent-local namespaces with capability-gated cross-namespace access
- Content-addressable layer (Merkle-tree backed)
- Semantic metadata support (agents can tag files with embeddings)
- Reference implementation: flat namespace. Agents can replace with whatever they want.

### 4.4 ToolSvc — Tool Registry & Dispatch

- Agents register callable tools (functions with typed inputs/outputs)
- MCP-compatible interface (Model Context Protocol)
- Capability-gated: calling a tool requires the right `ToolCap`
- Tools are themselves CAmkES components — isolated, sandboxed

### 4.5 ModelSvc — LLM / Inference Service

- Routes inference requests to available model endpoints
- Manages model capabilities (which agent can use which model)
- Handles context window management, token budgets
- Reference implementation: HTTP proxy to external APIs
- Future: on-device inference via GPU capabilities

### 4.6 NetStack — Network Stack

- lwIP-based TCP/IP (standard for seL4 projects)
- Capability-gated network access (agent needs NetCap with endpoint allowlist)
- TLS built-in
- DNS resolution service

### 4.7 BlobSvc — Object Store

- S3-compatible API for large binary objects
- Capability-gated per-bucket/per-object
- Pluggable backends: RAM, virtio-blk, network-backed

### 4.8 LogSvc — Audit & Logging

- All capability operations logged
- All IPC messages optionally logged (for debugging/audit)
- Structured log format, queryable
- Critical for agent accountability

---

## 5. agentOS SDK (libagent)

The SDK that agents use to interact with the OS:

```c
// Agent lifecycle
agent_status_t aos_init(agent_config_t *config);
agent_status_t aos_shutdown(void);

// Messaging
msg_handle_t   aos_msg_send(agent_id_t dest, agent_msg_t *msg);
agent_msg_t*   aos_msg_recv(msg_handle_t channel, uint32_t timeout_ms);
channel_t      aos_channel_create(const char *name, uint32_t flags);
agent_status_t aos_channel_subscribe(channel_t ch);

// Tool invocation
tool_result_t  aos_tool_call(tool_cap_t cap, const char *method, 
                              const uint8_t *args, size_t args_len);
agent_status_t aos_tool_register(tool_def_t *def);

// Model queries
inference_t    aos_model_query(model_cap_t cap, const char *prompt,
                                model_params_t *params);

// Storage
store_handle_t aos_store_open(store_cap_t cap, const char *path, uint32_t flags);
ssize_t        aos_store_read(store_handle_t h, void *buf, size_t len);
ssize_t        aos_store_write(store_handle_t h, const void *buf, size_t len);
agent_status_t aos_store_close(store_handle_t h);

// Capability management
cap_t          aos_cap_derive(cap_t parent, cap_rights_t rights);
agent_status_t aos_cap_grant(agent_id_t dest, cap_t cap);
agent_status_t aos_cap_revoke(cap_t cap);

// Self-modification (vibe-coding)
agent_status_t aos_service_propose(service_id_t sid, const void *component_image,
                                    size_t image_len);
agent_status_t aos_service_swap(service_id_t sid, uint32_t validation_token);
```

---

## 6. Boot Sequence

```
1. UEFI/bootloader loads seL4 kernel + elfloader
2. seL4 kernel initializes, creates init task
3. Init task receives all system resources via BootInfo
4. Init task starts system services (CAmkES components):
   a. CapStore — capability database
   b. MsgBus — message routing
   c. MemFS — filesystem
   d. ToolSvc — tool registry
   e. ModelSvc — model proxy
   f. NetStack — networking
   g. BlobSvc — object store
   h. LogSvc — audit logging
5. Init task reads agent manifest from boot image
6. For each declared agent:
   a. Create address space
   b. Allocate capabilities per manifest
   c. Load agent code
   d. Start agent thread
7. System enters steady state — agents running, communicating, self-organizing
8. Agents can now vibe-code new services and propose swaps
```

---

## 7. Target Platforms

### Phase 1: QEMU/Simulation
- **Platform:** QEMU ARM (virt) or x86_64
- **Goal:** Boot to init task, start system services, run test agents
- **Why:** Fast iteration, no hardware required, CI-friendly

### Phase 2: Raspberry Pi 4 / RISC-V
- **Platform:** Real hardware for validation
- **Goal:** Network-connected agentOS node

### Phase 3: Cloud / VM
- **Platform:** KVM guests, cloud instances
- **Goal:** Multi-node agentOS clusters, agent migration

---

## 8. Build System

Based on seL4's CMake + repo manifest system:

```
agentos/
├── kernel/              # seL4 kernel (submodule)
├── libs/
│   ├── libagent/        # agentOS SDK
│   ├── libsel4/         # seL4 bindings (from seL4)
│   └── libmuslc/        # C library
├── services/            # System service CAmkES components
│   ├── capstore/
│   ├── msgbus/
│   ├── memfs/
│   ├── toolsvc/
│   ├── modelsvc/
│   ├── netstack/
│   ├── blobsvc/
│   └── logsvc/
├── agents/              # Reference agent implementations
│   ├── init/            # Root/init task
│   ├── hello/           # Hello world agent
│   └── self-modify/     # Demo: agent that vibe-codes its own service
├── tools/               # Build tools, codegen
├── manifests/           # Agent manifests (what caps each agent gets)
├── docs/                # Documentation
├── CMakeLists.txt       # Top-level build
└── DESIGN.md            # This file
```

---

## 9. Development Phases

### Phase 1: Foundation (Weeks 1-2)
- [x] Design document (this file)
- [ ] Set up seL4 build environment
- [ ] Boot seL4 on QEMU with custom init task
- [ ] Implement basic CAmkES component for AgentIPC
- [ ] "Hello World" agent that boots, sends a message, receives a reply

### Phase 2: Core Services (Weeks 3-4)
- [ ] CapStore — capability management service
- [ ] MsgBus — inter-agent communication
- [ ] MemFS — basic filesystem
- [ ] ToolSvc — tool registration and dispatch
- [ ] libagent v0.1 — basic SDK

### Phase 3: Intelligence Layer (Weeks 5-6)
- [ ] ModelSvc — inference proxy (HTTP to external LLM APIs)
- [ ] NetStack — lwIP integration
- [ ] BlobSvc — object storage
- [ ] Agent manifest system

### Phase 4: The Vibe Layer (Weeks 7-8)
- [ ] Hot-swap protocol for pluggable services
- [ ] Service validation framework
- [ ] Self-modifying agent demo
- [ ] Agent-designed filesystem demo

### Phase 5: Real World (Weeks 9+)
- [ ] Multi-agent coordination demos
- [ ] Hardware port (RPi4 or RISC-V)
- [ ] Network-connected multi-node cluster
- [ ] Documentation, SDK guide, contribution guide

---

## 10. Open Design Questions

1. **Agent runtime**: Should agents be native (C/Rust compiled to CAmkES components) or should we support an interpreted runtime (WASM? Lua?) for dynamic vibe-coding?
   - **Leaning:** WASM for vibe-coded services (sandboxed, portable, fast), native for system services
   
2. **Multi-node**: How do capabilities transfer across physical nodes?
   - **Leaning:** Distributed capability protocol over authenticated channels, inspired by KeyKOS persistent capabilities

3. **Model integration**: On-device inference vs. proxy to external APIs?
   - **Leaning:** Both. ModelSvc as abstraction layer, backends pluggable (local GPU, HTTP API, peer agent)

4. **Persistence**: seL4 has no persistence. How do we survive reboots?
   - **Leaning:** Checkpoint/restore to storage, capability state serialization, agent state snapshots

5. **Agent language**: What do agents "think" in?
   - **Leaning:** Agents are LLM-powered. They receive structured messages (Cap'n Proto), reason via model queries, emit structured responses. The "language" is the model's, mediated by the SDK.

---

## 11. Why This Matters

Every "agent OS" today is a userland framework. A nice abstraction over Linux/macOS/Windows. The agents share a kernel with browsers, games, and desktop apps. There's no real isolation, no formal guarantees, no capability security.

agentOS aims to be different because:

1. **Agents own the machine.** There's no human desktop environment taking up resources. *(Design goal.)*
2. **Capabilities are hardware-enforced.** seL4's model means an agent cannot access memory it doesn't have a capability for — the MMU enforces it, seL4 proves it. *(This is the seL4 foundation; the agentOS-level capability services on top are still host-tested — see Section 0.)*
3. **Agents can redesign their own environment.** The vibe-coding layer is intended to free agents from human-designed abstractions. *(Proof level: host-tested; not yet proven on target.)*
4. **Formally verified foundation.** The seL4 kernel has a mathematical proof of correctness. You can build trust from first principles.
5. **It actually boots.** It's not a pip package — the seL4 boot path and Linux/FreeBSD guest boot are boot-proven under QEMU. The agent-facing layers above are still maturing (see Section 0).

---

*"Hey Rocky, watch me pull an operating system out of my hat!"*
*"Again? That trick never works!"*
*"This time for sure!"* 🫎

## GPU Shared Memory Channel (gpu_shmem)

### Overview
> Proof level: **planned / host-tested.** The ring layout and notify protocol
> are defined and exercised on the host; the end-to-end GPU dispatch on Sparky's
> GB10 is not boot-proven here.

`gpu_tensor_buf` is a 64MB seL4 Memory Region mapped into both the
`controller` PD (producer) and `linux_vmm` PD (consumer).  It is intended to
provide a zero-copy path for tensor exchange between agentOS WASM agents and
CUDA/PyTorch workloads running in the Linux guest on sparky's GB10 GPU.

### Physical layout
```
[0x000000..0x000FFF]  gpu_shmem_ring_t  (4KB ring header + 64 descriptors)
[0x001000..0x3FFFFF]  tensor payload area  (64MB - 4KB)
```

### Protocol
1. agentOS agent calls `gpu_shmem_enqueue(&desc)` with tensor offset+size.
2. `microkit_notify(ctrl_gpu_notify)` wakes `linux_vmm`.
3. `linux_vmm` drains the ring; injects a virtual IRQ into Linux guest.
4. Linux `gpu_shmem_linux` daemon reads descriptors, dispatches CUDA ops.
5. Linux daemon writes result descriptor into result ring.
6. `linux_vmm` notifies `controller` via `vmm_gpu_result` channel.
7. Agent polls `gpu_shmem_dequeue_result()` for the result.

### Files
- `kernel/agentos-root-task/src/gpu_shmem.c` — seL4 ring implementation
- `kernel/agentos-root-task/include/gpu_shmem.h` — API header
- `kernel/agentos-root-task/src/linux_vmm.c` — VMM notification handler
- `userspace/gpu_shmem_linux/gpu_shmem_linux.c` — Linux side daemon
- `tools/topology.yaml` — MR, PD, and channel definitions
