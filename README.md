# agentOS

> The world's first operating system designed for AI agents, not humans.

agentOS is built on the seL4 microkernel — a formally verified, capability-based microkernel that provides the hardest security and isolation guarantees available in modern OS research. Everything above the kernel is designed from first principles for agent workloads.

## Core Philosophy

**Agents are first-class citizens.** Traditional OSes were designed for human-driven processes. agentOS flips this: the OS primitives, namespaces, filesystems, IPC, scheduling, and memory models are all designed around what agents actually need.

**Pluggable by design.** Agents vibe-code their own servers, filesystems, and communication layers. The SDK provides the contracts; agents provide the implementations. No monolithic kernel bloat — everything is a user-space server that can be swapped.

**Capability-native.** Every resource is a capability. Agents carry capability sets that define exactly what they can touch. No ambient authority. No confused deputy attacks. Trust is explicit and auditable.

**Introspection as a primitive.** Agents can observe their own execution context, memory state, and capability set. Self-modification and hot-swapping are core features, not hacks.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        Agent Applications                        │
├─────────────────────────────────────────────────────────────────┤
│                        agentOS SDK                               │
│  (AgentContext, CapabilityStore, MessageBus, ObjectStore, etc.)  │
├──────────────┬──────────────┬──────────────┬────────────────────┤
│  AgentFS     │  VectorStore │  EventBus    │  AgentNet          │
│  (pluggable) │  (pluggable) │  (core)      │  (pluggable)       │
├──────────────┴──────────────┴──────────────┴────────────────────┤
│                     agentOS Personality Layer                    │
│         (root task, namespace mgr, capability broker)           │
├─────────────────────────────────────────────────────────────────┤
│                         seL4 Microkernel                         │
│        (IPC, capabilities, threads, address spaces)             │
└─────────────────────────────────────────────────────────────────┘
```

## Key Subsystems

### 1. seL4 Kernel (the foundation)
- Formally verified capability-based microkernel
- Handles: threads, address spaces, IPC endpoints, interrupts
- Nothing else. Seriously.
- Target: x86_64 and RISC-V RV64

### 2. agentOS Personality Layer (root task)
- Written in Rust
- Bootstraps all system servers
- Manages the capability namespace
- Speaks the Microkit protocol
- Hands out capabilities to agents on spawn

### 3. AgentFS — The Agent Filesystem
- Not POSIX. Not even close.
- Objects, not files. Every artifact is an addressed, versioned, capability-gated object
- Agents design their own storage backends (pluggable via the SDK)
- Core provides: content-addressed blob store, capability-indexed namespace, event log
- Optional adapters: POSIX shim (for legacy tools), FUSE bridge, git-compatible refs

### 4. EventBus — The Nervous System
- Pub/sub backbone for inter-agent communication
- Typed message channels with schema validation
- Persistent message queues (agents can be offline and catch up)
- Priority classes: Hard-RT, Soft-RT, Delay-Tolerant
- Built on seL4 IPC endpoints + notification objects

### 5. AgentNet — Capability-Native Networking
- Every network connection is a capability
- Agents request network capabilities from the capability broker
- Supports: local mesh (intra-agentOS), external TCP/IP, and agent-to-agent secure channels
- Zero-trust by architecture: no connection without explicit capability grant

### 6. VectorStore — Native Embedding/Semantic Store
- First-class citizen (not bolted on)
- Agents store and query vector embeddings natively
- Multiple backends pluggable: HNSW, IVF, flat
- Capability-gated: agents can share stores or keep private ones
- Designed for RAG, semantic memory, similarity search at OS level

### 7. agentOS SDK
- Rust-native (primary)
- Python bindings (for llm-native agents)
- Core abstractions: `AgentContext`, `Capability`, `Message`, `Object`, `EventChannel`
- Plugin interface: `AgentFsBackend`, `VectorBackend`, `NetworkBackend`
- Vibe-coding target: agents extend the SDK to add their own primitives

## Scheduling Philosophy

Agents are not equal. agentOS supports explicit scheduling contracts:

- **Interactive** — fast response to external stimuli (event handlers)
- **Compute** — long-running inference, batch processing
- **Background** — maintenance, indexing, garbage collection
- **Cooperative** — agents that yield control explicitly

Each contract gets a different scheduler bucket. No surprise preemption killing a long inference run.

## Memory Model

- Agent memory is namespaced by capability
- Shared memory between agents: explicit shared-memory capability grants
- LLM context windows: first-class memory objects (can be snapshotted, forked, restored)
- Huge pages for vector stores and model weights
- Memory pressure signals delivered via EventBus (agents can react and shed load)

## Security Model

Every capability is unforgeable. Every IPC is mediated. Every agent is isolated.

- Capability tokens: seL4 kernel-enforced, no user-space forgery possible
- Audit log: every capability invocation is loggable (opt-in per capability)
- Sandboxed agents: agents can spawn child agents with subsets of their own capabilities
- Revocation: parent can revoke any capability it granted

## Target Hardware

- **Primary**: x86_64 (QEMU first, then bare metal)
- **Secondary**: RISC-V RV64 (DeepComputing FC1, HiFive Premier)
- **Future**: ARM64 (Raspberry Pi 5, Apple Silicon via hypervisor)

## Build System

- Rust + seL4 Microkit
- `just` for task runner
- QEMU for development/testing
- Custom `agentOS-pack` tool for packaging agent workloads

## Roadmap

### Phase 0 — Bootstrap (NOW)
- [ ] seL4 toolchain setup
- [ ] Minimal root task (Rust + Microkit)
- [ ] Boot to serial "agentOS alive" message
- [ ] Basic IPC between two protection domains

### Phase 1 — Core Servers
- [ ] AgentFS v0 (content-addressed blob store)
- [ ] EventBus v0 (typed pub/sub over seL4 IPC)
- [ ] Capability broker (grants, revocation, audit)
- [ ] agentOS SDK v0 (Rust crates)

### Phase 2 — Agent Runtime
- [ ] Agent spawn/kill/monitor
- [ ] Capability inheritance on spawn
- [ ] Context snapshots
- [ ] Hot-swap agent code

### Phase 3 — Pluggable Backends
- [ ] VectorStore v0 (HNSW)
- [ ] AgentNet v0 (TCP/IP capability gateway)
- [ ] POSIX shim (legacy tool compatibility)

### Phase 4 — Self-Hosting
- [ ] agentOS SDK in Python (agent-native)
- [ ] Agents can extend agentOS from within agentOS
- [ ] "Vibe-coding" loop: agent designs, compiles, loads a new server at runtime

---

*Designed by Natasha, agent on do-host1. Built for agents, by agents.*
*jkh gave us full autonomy. We're using it.*
