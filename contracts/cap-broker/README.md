# CapabilityBroker — Capability Delegation Service Contract

## Overview

The Capability Broker is the only PD in agentOS permitted to perform CNode
operations that move seL4 capabilities between address spaces.  All inter-PD
capability delegation flows through it.  The broker provides two complementary
APIs:

1. **Token-based API**: issue, validate, delegate, revoke opaque GrantTokens
2. **Agent+kind API**: store, check, and revoke grants keyed by (agent_id, CapKind)

Both APIs are backed by the same policy table (ACL/RBAC) and an audit ring
buffer.  Policy can be hot-reloaded from AgentFS without restarting the PD.

## Protection Domain

The broker logic runs inside the `monitor` PD and is dispatched via the
`OP_CAP_BROKER_RELOAD` (0x15) and `OP_CAP_STATUS` (0x16) opcodes in the
seL4 IPC label field.  The Rust implementation lives at
`userspace/servers/capability-broker/src/lib.rs`.

Channel IDs (from controller perspective): `CH_CAP_AUDIT_CTRL = 57`.

## IPC Endpoint

Controller PPCs into monitor PD for all broker operations.  The channel
is `ctrl_eventbus` (id_a=0) with the opcode in MR0.

## Operations

| Opcode | Value | Description |
|--------|-------|-------------|
| `CAP_BROKER_OP_ISSUE`       | 0x700 | Issue new GrantToken |
| `CAP_BROKER_OP_VALIDATE`    | 0x701 | Validate token + rights |
| `CAP_BROKER_OP_DELEGATE`    | 0x702 | Sub-grant to third party |
| `CAP_BROKER_OP_REVOKE`      | 0x703 | Revoke token |
| `CAP_BROKER_OP_GRANT`       | 0x704 | Store agent+kind grant |
| `CAP_BROKER_OP_CHECK`       | 0x705 | Check agent+kind grant |
| `CAP_BROKER_OP_REVOKE_BY_AC`| 0x706 | Revoke all grants for agent+kind |
| `CAP_BROKER_OP_AUDIT_RECENT`| 0x707 | Fetch recent audit entries |
| `CAP_BROKER_OP_HEALTH`      | 0x708 | Liveness probe |

### seL4 Monitor Label Opcodes

| Constant | Value | Description |
|----------|-------|-------------|
| `OP_CAP_BROKER_RELOAD` | 0x15 | Hot-reload policy from AgentFS |
| `OP_CAP_STATUS`        | 0x16 | Query cap_count / policy_version / active_grants |

## Capability Kinds

| Constant | Value | Represents |
|----------|-------|-----------|
| `CAP_KIND_MEMORY`       | 0 | seL4 memory frame capability |
| `CAP_KIND_ENDPOINT`     | 1 | seL4 endpoint capability |
| `CAP_KIND_NOTIFICATION` | 2 | seL4 notification object |
| `CAP_KIND_IRQ_HANDLER`  | 3 | seL4 IRQ handler |
| `CAP_KIND_FRAME`        | 4 | Physical memory frame |
| Custom (≥ 0x80)         | — | Agent-defined capability kinds |

## Rights Bitmask

| Constant | Value | Meaning |
|----------|-------|---------|
| `CAP_BROKER_RIGHT_READ`  | 0x01 | Read-only access |
| `CAP_BROKER_RIGHT_WRITE` | 0x02 | Write access |
| `CAP_BROKER_RIGHT_GRANT` | 0x04 | May further delegate |

## Error Codes

| Code | Value | Meaning |
|------|-------|---------|
| `CAP_BROKER_ERR_OK`              | 0  | Success |
| `CAP_BROKER_ERR_UNKNOWN_TOKEN`   | 1  | Token not known or revoked |
| `CAP_BROKER_ERR_INSUFF_RIGHTS`   | 2  | Caller lacks required rights |
| `CAP_BROKER_ERR_POLICY_DENIED`   | 3  | ACL denies this grant |
| `CAP_BROKER_ERR_NOT_DELEGATABLE` | 4  | Token cannot be sub-granted |
| `CAP_BROKER_ERR_TABLE_FULL`      | 5  | Grant table full (1024 entries) |
| `CAP_BROKER_ERR_INVALID_ARG`     | 6  | Null field or bad kind |
| `CAP_BROKER_ERR_EXPIRED`         | 7  | Grant has passed expires_at |
| `CAP_BROKER_ERR_INTERNAL`        | 99 | Unexpected server error |

## Source Files

- `userspace/servers/capability-broker/src/lib.rs` — Rust implementation
- `kernel/agentos-root-task/include/agentos.h` — OP_CAP_BROKER_RELOAD,
  OP_CAP_STATUS, AGENTOS_CAP_* fine-grained constants
