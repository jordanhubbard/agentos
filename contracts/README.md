# agentOS IPC Contract Framework

This directory contains the formal seL4 IPC API contracts for every
protection domain (PD) service in agentOS.  Each subdirectory defines the
wire-level ABI for one service: opcodes, packed request/reply structs, error
codes, and a prose description of the PD's role, channels, and shared memory
conventions.

## What a Contract Is

A contract is a binding specification of the IPC protocol between a caller PD
and a server PD.  It answers:

1. What is the opcode space?  (OPCODE defines as `uint32_t` in MR0)
2. What are the packed structs for every request and reply?
3. What error codes can be returned in MR0 of every reply?
4. Which shared memory regions does the server read or write, and at what
   offsets?
5. Which seL4 channel does a caller use to reach the server?

Contracts are written in C so they can be directly included by any PD
without a foreign-language translation step.

## File Structure

Each service directory contains exactly two files:

```
contracts/<service-name>/
    interface.h   — C header: opcodes, structs, error codes
    README.md     — prose description, PD topology, channel mapping
```

## Design Rules for interface.h Files

All `interface.h` files follow these invariants:

- `#pragma once` include guard (no `#ifndef` guards)
- Version: `#define <SERVICE>_INTERFACE_VERSION <N>`
- All opcodes: `#define <SERVICE>_OP_<NAME>  <hex_value>u`
- All error codes: `#define <SERVICE>_ERR_<NAME>  <decimal>u`
  with `_ERR_OK = 0` and `_ERR_INTERNAL = 99`
- All structs: `__attribute__((packed))` with `typedef` alias ending in `_t`
- Request structs: `typedef struct <service>_<op>_req { uint32_t opcode; ... } ... <service>_<op>_req_t;`
- Reply structs: `typedef struct <service>_<op>_rep { uint32_t status; ... } ... <service>_<op>_rep_t;`
- MR0 is always `opcode` on request and `status` on reply
- Shared memory references use `uint32_t *_shmem_offset` + `uint32_t *_len` pairs

## seL4 IPC Primitives

All services use `seL4_Call` (caller blocks) and `seL4_Reply` (server
unblocks caller).  The label field of `seL4_MessageInfo_t` carries the
opcode.  Message registers (MRs) carry the packed struct fields.  seL4
provides 120 bytes of inline MR space; structs that exceed this use a
shared memory region (MR mapped by the Microkit system description).

A passive PD blocks in `seL4_Recv` between requests.  A Protected Procedure
Call (PPC, `pp=true` in topology channels) guarantees the server runs at
its own priority when the caller PPCs in — this prevents priority inversion.

## Service Index

| Directory | Status | Priority | Description |
|-----------|--------|----------|-------------|
| [agentfs](agentfs/) | Implemented | 150 | Agent-native object store (content-addressed, versioned, vector search) |
| [cap-broker](cap-broker/) | Implemented | (monitor PD) | Capability delegation and ACL enforcement |
| [capstore](capstore/) | Implemented | (monitor PD) | Semantic capability database and audit log |
| [event-bus](event-bus/) | Implemented | 200 | Pub/sub event routing between PDs |
| [logsvc](logsvc/) | Implemented | (controller PD) | Structured audit logging service |
| [memfs](memfs/) | Implemented | (controller PD) | In-memory virtual filesystem (storage.v1 ABI) |
| [modelsvc](modelsvc/) | Implemented | (controller PD) | LLM inference proxy (OpenAI-compatible) |
| [msgbus](msgbus/) | Implemented | 200 | Named channel pub/sub and point-to-point messaging |
| [net-server](net-server/) | Partially implemented | 160 | Virtual NIC and TCP/IP stack (smoltcp integration pending) |
| [toolsvc](toolsvc/) | Implemented | (controller PD) | Tool registry and MCP-compatible dispatch |

## Protection Domain Topology (summary)

The full topology is defined in `tools/topology.yaml`.  Key PDs and their
priorities:

```
monitor          (priority 254) — system supervisor, cap broker
time_partition   (priority 250) — time budget enforcement
oom_killer       (priority 245) — memory pressure response
event_bus        (priority 200) — event routing (also MsgBus)
snapshot_sched   (priority 180) — periodic WASM checkpoint
vibe_engine      (priority 140) — hot-swap WASM execution engine
net_server / serial_pd (priority 160) — I/O services
agentfs          (priority 150) — object store
mem_profiler     (priority 108) — heap leak detection
init_agent       (priority 100) — bootstrap and spawn
linux_vmm        (priority 100) — CUDA/PyTorch GPU VMM
controller       (priority  50) — system coordinator
worker_N         (priority  80) — agent worker pool (8 workers)
swap_slot_N      (priority  75) — WASM execution sandboxes (4 slots)
```

## Opcode Namespace Allocation

To avoid collisions, opcode ranges are allocated per service:

| Range | Service |
|-------|---------|
| 0x0001 – 0x00FF | Legacy seL4 label opcodes (agentos.h MSG_*) |
| 0x0010 – 0x001F | VM manager (OP_VM_*) |
| 0x0040 – 0x005F | VibeEngine (OP_VIBE_*) |
| 0x0050 – 0x005F | WatchdogPD (OP_WD_*) |
| 0x0050 – 0x0053 | CapAuditLog (OP_CAP_LOG*) |
| 0x0060 – 0x0063 | QuotaPD (OP_QUOTA_*) |
| 0x0080 – 0x0083 | TraceRecorder (OP_TRACE_*) |
| 0x00B0 – 0x00BA | NetServer (OP_NET_*) |
| 0x00C0 – 0x00C3 | CapPolicy (OP_CAP_POLICY_*) |
| 0x00E0 | FaultHandler extended (OP_FAULT_POLICY_SET) |
| 0x0100 – 0x010B | MsgBus (MSGBUS_OP_*) |
| 0x0200 – 0x0206 | CapStore (CAPSTORE_OP_*) |
| 0x0300 – 0x0304 | LogSvc (LOGSVC_OP_*) |
| 0x0400 – 0x0406 | ToolSvc (TOOLSVC_OP_*) |
| 0x0500 – 0x0505 | ModelSvc (MODELSVC_OP_*) |
| 0x0500 | NetServer HTTP POST proxy (NET_OP_HTTP_POST) |
| 0x0600 – 0x0606 | EventBus (EVENTBUS_OP_*) |
| 0x0700 – 0x0708 | CapBroker (CAP_BROKER_OP_*) |
| 0x0030 – 0x0036 | AgentFS (AGENTFS_OP_*) |

Note: ModelSvc uses 0x500 range and NetServer HTTP POST is also at 0x500.
These are dispatched by different PDs (controller vs net_server) so there
is no conflict at the seL4 channel level.

## Adding a New Contract

1. Create a directory: `contracts/<service-name>/`
2. Write `interface.h` following the design rules above
3. Write `README.md` with: overview, PD identity, channel mapping, operation
   table, error code table, and source file references
4. Allocate an opcode range in this index
5. Open a PR updating this README's service index and opcode table

## Source References

- `tools/topology.yaml` — authoritative PD and channel definitions
- `kernel/agentos-root-task/include/agentos.h` — system-wide opcode constants
- `kernel/agentos-root-task/include/net_server.h` — NetServer OP_NET_* values
- `services/abi/agentos_service_abi.h` — WASM hot-swap service ABI
- `userspace/servers/*/src/lib.rs` — Rust userspace server implementations
