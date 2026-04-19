# MemFS — Virtual Filesystem Service Contract

## Overview

MemFS is an in-memory, per-agent-namespace filesystem.  It is the reference
storage service and the provider of the `storage.v1` WASM ABI.  Features:

- Flat namespace keyed by path strings up to 256 characters
- Capability-gated access: each file tracks an owner AgentID (32 bytes)
- Semantic tagging: up to 4 human/agent-readable tags per file
- Opcode space shared with the storage.v1 hot-swap ABI (see
  `services/abi/agentos_service_abi.h`), so WASM replacements are
  drop-in compatible

The service is intentionally small (64 files, 4KB each) for the seL4
prototype.  Replace it by vibe-coding a storage.v1 implementation that
delegates to AgentFS or an external object store.

## Protection Domain

MemFS is currently implemented as a library linked into the controller PD.
The long-term plan is to promote it to a standalone passive PD at priority
~150 (between agentfs at 150 and console_mux at 160).

## IPC Endpoint

Channel assignment is controller-relative.  The controller handles MemFS
dispatch internally; external agents call the controller which proxies to the
MemFS library.

## Operations

The opcode space is aligned with `agentos_service_abi.h` STORAGE_OP_*:

| Opcode | Value | Description |
|--------|-------|-------------|
| `MEMFS_OP_WRITE`     | 0x30 | Create or overwrite a file |
| `MEMFS_OP_READ`      | 0x31 | Read file contents |
| `MEMFS_OP_DELETE`    | 0x32 | Delete file |
| `MEMFS_OP_STAT`      | 0x33 | Get file metadata |
| `MEMFS_OP_LIST`      | 0x34 | List files by prefix |
| `MEMFS_OP_STAT_SVC`  | 0x20 | Service statistics |
| `MEMFS_OP_TAG`       | 0x35 | Attach semantic tag to file |
| `MEMFS_OP_FIND_BY_TAG` | 0x36 | List files with a tag |
| `MEMFS_OP_HEALTH`    | 0x37 | Liveness probe |

## Error Codes

| Code | Value | Meaning |
|------|-------|---------|
| `MEMFS_ERR_OK`          | 0  | Success |
| `MEMFS_ERR_INVALID_ARG` | 1  | Null path or empty data |
| `MEMFS_ERR_NOT_FOUND`   | 2  | File does not exist |
| `MEMFS_ERR_EXISTS`      | 3  | File already exists |
| `MEMFS_ERR_TOO_BIG`     | 4  | Data exceeds 4KB limit |
| `MEMFS_ERR_NOMEM`       | 5  | No free file slots (max 64) |
| `MEMFS_ERR_DENIED`      | 6  | Caller does not own the file |
| `MEMFS_ERR_TOO_MANY_TAGS` | 7 | File already has 4 tags |
| `MEMFS_ERR_INTERNAL`    | 99 | Unexpected server error |

## Shared Memory Conventions

Large data (file contents, path lists) is transferred via a shared memory
region (a Microkit MR).  The request struct encodes `data_shmem_offset` and
`data_len` relative to the start of the caller's MR.  The server reads or
writes into that region before replying.

## Source Files

- `services/memfs/memfs.c` — implementation
- `services/abi/agentos_service_abi.h` — WASM ABI compatibility opcodes
