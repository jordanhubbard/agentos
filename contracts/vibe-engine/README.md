# VibeEngine — WASM Hot-Swap Service Contract

## Overview

VibeEngine manages the hot-swap lifecycle of WASM service components within
agentOS.  Agents generate or retrieve new WASM implementations and submit
them to VibeEngine, which validates, tests, and atomically swaps them into
live service slots — zero-downtime, with automatic rollback on failure.

The VibeEngine is distinct from vibeOS (which manages full guest OS instances):
- **vibe-engine**: hot-swap individual WASM service components (memfs, toolsvc, etc.)
- **vibeos**: create/destroy/configure full guest OS instances

## Hot-Swap Pipeline

```
Agent writes WASM to staging region
    │
    ▼  VSWAP_OP_PROPOSE
VibeEngine assigns proposal_id, records WASM metadata
    │
    ▼  VSWAP_OP_VALIDATE
VibeEngine checks WASM magic, exports (init/handle_ppc/health_check/notified),
agentos.capabilities section, capability declarations
    │
    ▼  VSWAP_OP_COMMIT
Controller copies WASM into swap slot, loads via wasm3, runs conformance tests
On success: redirects IPC channel; keeps old slot warm as rollback
    │
    ▼  VSWAP_OP_STATUS (polling)
Agent polls until VSWAP_STATE_ACTIVE or VSWAP_STATE_REJECTED
    │
    ▼  VSWAP_OP_ROLLBACK (optional, within rollback window)
Controller redirects channel back to warm rollback slot
```

## Protection Domain

`vibe_engine` is a passive PD (priority 140) defined in `tools/topology.yaml`.
It maps the `vibe_code` MR (4MB) and `vibe_state` MR (64KB).

| Caller | Channel |
|--------|---------|
| controller | ctrl_vibe (id_a=40) |
| init_agent | init_vibe (id_a=2, pp_a=true) |

## Swappable Services

| ID | Constant | Swappable? |
|----|----------|-----------|
| 0 | `VSWAP_SVC_EVENTBUS` | No (critical path in v0.1) |
| 1 | `VSWAP_SVC_MEMFS`    | Yes |
| 2 | `VSWAP_SVC_TOOLSVC`  | Yes |
| 3 | `VSWAP_SVC_MODELSVC` | Yes |
| 4 | `VSWAP_SVC_AGENTFS`  | Yes |
| 5 | `VSWAP_SVC_LOGSVC`   | Yes |

## Operations

| Opcode | Value | Description |
|--------|-------|-------------|
| `VSWAP_OP_PROPOSE`      | 0x40 | Submit WASM proposal |
| `VSWAP_OP_VALIDATE`     | 0x41 | Run validation on proposal |
| `VSWAP_OP_COMMIT`       | 0x42 | Initiate live swap |
| `VSWAP_OP_STATUS`       | 0x43 | Query proposal state |
| `VSWAP_OP_ROLLBACK`     | 0x44 | Revert to previous version |
| `VSWAP_OP_HEALTH`       | 0x45 | Liveness probe |
| `VSWAP_OP_LIST_SERVICES`| — | Enumerate swappable services |

(Note: opcodes also defined as `OP_VIBE_*` in `agentos.h` for kernel dispatch)

## Staging Region

Agents write WASM binaries into the `vibe_code` staging MR (4MB, `VSWAP_SHMEM_SIZE`)
before calling `VSWAP_OP_PROPOSE`.  Only one pending proposal per agent is
supported; a second PROPOSE before COMMIT/ROLLBACK returns `VSWAP_ERR_BUSY`.

## Source Files

- `kernel/agentos-root-task/src/vibe_engine.c` — PD implementation
- `kernel/agentos-root-task/include/agentos.h` — OP_VIBE_* constants
- `userspace/servers/vibe-engine/src/wasm_validator.rs` — Rust WASM validator
- `userspace/sim/` — simulation test harness
