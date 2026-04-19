# CapStore — Capability Database Service Contract

## Overview

CapStore is the agentOS semantic capability database.  It sits above seL4's
own CNode/CDT machinery and provides application-level capability tracking for
the AI workload layer:

- Every capability derivation, grant, and revocation is recorded here
- Cascading revocation: revoking a cap automatically revokes all descendants
- Capabilities are typed (TOOL, MODEL, MEMORY, MSG, STORE, SPAWN, NET, SELF,
  SERVICE) with a rights bitmask (READ, WRITE, GRANT)
- Rights are monotonically decreasing: derived caps cannot escalate rights
- All operations are audited (tied to logsvc via the cap_audit_log MR)

## Protection Domain

CapStore logic runs inside the `monitor` PD and is also exposed through the
kernel capability audit log PD (`cap_audit_log`).  The `controller` PD holds
the primary PPC endpoint into both.

Channel IDs from the controller's perspective:

| Purpose | Channel constant |
|---------|-----------------|
| CapStore admin ops | `CH_CAP_AUDIT_CTRL` = 57 |
| Policy hot-reload  | Handled by cap_broker via monitor |

## IPC Endpoint

All capability operations are serialized over seL4_Call.  The init task uses
channel 5 (`CH_CAP_AUDIT_INIT`) from its own perspective.

## Operations

| Opcode | Value | Description |
|--------|-------|-------------|
| `CAPSTORE_OP_REGISTER`    | 0x200 | Create a root capability for an agent |
| `CAPSTORE_OP_DERIVE`      | 0x201 | Derive a sub-capability (rights subset) |
| `CAPSTORE_OP_REVOKE`      | 0x202 | Revoke cap and all descendants |
| `CAPSTORE_OP_CHECK`       | 0x203 | Verify a cap is valid for a caller |
| `CAPSTORE_OP_QUERY_OWNER` | 0x204 | List all live caps for an agent |
| `CAPSTORE_OP_INFO`        | 0x205 | Fetch full metadata for a cap |
| `CAPSTORE_OP_HEALTH`      | 0x206 | Liveness probe |

## Capability Types

| Constant | Value | Grants access to |
|----------|-------|-----------------|
| `CAPSTORE_CAP_TOOL`    | 0x01 | Invoke a registered tool via ToolSvc |
| `CAPSTORE_CAP_MODEL`   | 0x02 | Call an inference endpoint via ModelSvc |
| `CAPSTORE_CAP_MEMORY`  | 0x03 | Heap allocation budget |
| `CAPSTORE_CAP_MSG`     | 0x04 | Publish/subscribe on MsgBus |
| `CAPSTORE_CAP_STORE`   | 0x05 | Read/write objects in AgentFS |
| `CAPSTORE_CAP_SPAWN`   | 0x06 | Spawn child agents |
| `CAPSTORE_CAP_NET`     | 0x07 | Network access via NetServer |
| `CAPSTORE_CAP_SELF`    | 0x08 | Introspect own capabilities |
| `CAPSTORE_CAP_SERVICE` | 0x09 | Bind to a named service endpoint |

## Error Codes

| Code | Value | Meaning |
|------|-------|---------|
| `CAPSTORE_ERR_OK`          | 0  | Success |
| `CAPSTORE_ERR_INVALID_ARG` | 1  | Bad opcode or null field |
| `CAPSTORE_ERR_NOT_FOUND`   | 2  | cap_id does not exist |
| `CAPSTORE_ERR_REVOKED`     | 3  | Capability has been revoked |
| `CAPSTORE_ERR_DENIED`      | 4  | Rights escalation attempted |
| `CAPSTORE_ERR_NOMEM`       | 5  | Entry table full (max 4096) |
| `CAPSTORE_ERR_INTERNAL`    | 99 | Unexpected server error |

## Shared Memory

The `audit_log` MR (32KB, see topology.yaml) is mapped into the cap_audit_log
PD.  `CAPSTORE_OP_QUERY_OWNER` and `CAPSTORE_OP_INFO` write results into the
caller's own MR.

## Source Files

- `services/capstore/capstore.c` — implementation
- `kernel/agentos-root-task/src/cap_audit_log.c` — seL4 PD wrapper
- `kernel/agentos-root-task/include/agentos.h` — opcode constants
  (`OP_CAP_LOG`, `OP_CAP_LOG_STATUS`, `OP_CAP_LOG_DUMP`, `OP_CAP_ATTEST`)
- `userspace/servers/capability-broker/src/lib.rs` — Rust userspace broker
