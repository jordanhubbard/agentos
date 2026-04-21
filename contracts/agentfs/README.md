# AgentFS — Agent Object Store Service Contract

## Overview

AgentFS is the agent-native persistent object store.  It is not POSIX.
Every stored item is an Object with rich structure:

- 32-byte content-addressed ID (BLAKE3 for blobs, UUID for mutable)
- Schema type tag (e.g. `agentOS::InferenceResult`, `agentOS::AgentState`)
- Version counter (mutable objects append new versions)
- Capability tag: only callers holding a matching seL4 badge can read
- Optional float32 vector embedding for semantic similarity search
- Lifecycle state: LIVE, TOMBSTONE (soft-deleted), EVICTED (cold tier)

AgentFS is the foundation for agent state persistence, snapshot/restore,
and vector-based retrieval-augmented generation (RAG).

## Protection Domain

`agentfs` is a standalone passive PD (priority 150) as defined in
`tools/topology.yaml`.  It maps the `agentfs_store` MR (2MB) for its
hot object tier.  `snapshot_sched` writes checkpoint data into the same
MR via the `snap_agentfs_write` channel.

## IPC Endpoint

| Caller PD | Channel to agentfs |
|-----------|--------------------|
| controller | 5 (ctrl_agentfs, pp_a=true) |
| snapshot_sched | 5 (snap_agentfs_write) |

## Operations

| Opcode | Value | Description |
|--------|-------|-------------|
| `AGENTFS_OP_PUT`    | 0x30 | Write / create an object |
| `AGENTFS_OP_GET`    | 0x31 | Read object by ID |
| `AGENTFS_OP_QUERY`  | 0x32 | List/filter objects |
| `AGENTFS_OP_DELETE` | 0x33 | Soft-delete (tombstone) |
| `AGENTFS_OP_VECTOR` | 0x34 | Vector similarity query |
| `AGENTFS_OP_STAT`   | 0x35 | Object metadata |
| `AGENTFS_OP_HEALTH` | 0x36 | Liveness probe |

## Error Codes

| Code | Value | Meaning |
|------|-------|---------|
| `AGENTFS_ERR_OK`           | 0  | Success |
| `AGENTFS_ERR_NO_CAP`       | 1  | Caller badge does not match cap_tag |
| `AGENTFS_ERR_NOT_FOUND`    | 2  | Object not found |
| `AGENTFS_ERR_NO_SPACE`     | 3  | Hot store full (256 objects / 256KB) |
| `AGENTFS_ERR_TYPE_MISMATCH`| 4  | schema_type conflict on mutable update |
| `AGENTFS_ERR_INVALID_ARG`  | 5  | Null pointer or zero size |
| `AGENTFS_ERR_INTERNAL`     | 99 | Unexpected server error |

## Shared Memory: agentfs_store MR

The `agentfs_store` MR (2MB, vaddr 0x5000000 in agentfs PD) serves dual
roles:

1. Hot object blob storage (first 256KB used by the hot index)
2. IPC transfer buffer: PUT/GET/QUERY/VECTOR operands pass by store offset

The caller maps the same MR (or a portion of it shared by the controller)
and uses `data_store_offset` fields to indicate where data resides before
calling.

## Event Emission

AgentFS emits events to the `event_bus` PD on every mutation:
- `EVT_OBJECT_CREATED` (0x0411): new object stored
- `EVT_OBJECT_DELETED` (0x0412): object tombstoned
- `EVT_OBJECT_EVICTED` (0x0413): object moved to cold tier

## Source Files

- `kernel/agentos-root-task/src/agentfs.c` — seL4 PD implementation
- `kernel/agentos-root-task/include/agentos.h` — EVT_OBJECT_* constants
- `tools/topology.yaml` — agentfs PD and channel definitions
