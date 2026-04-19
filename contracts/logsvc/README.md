# LogSvc — Audit Logging Service Contract

## Overview

LogSvc is the centralized, structured logging and audit service for agentOS.
It maintains a 16 384-entry circular ring buffer of structured log entries
and exposes them to agents for introspection.  Key properties:

- All capability operations (AUDIT level) are always recorded, ignoring the
  minimum level filter
- Structured entries include: sequence number, timestamp, AgentID, level,
  component name, and message
- Agents can query the log to read their own history
- The log is the system's long-term memory — agent LLMs inspect it directly

## Protection Domain

LogSvc is implemented as a library linked into the controller PD.  The
`audit_log` shared memory region (32KB, see topology.yaml) is mapped into
the cap_audit_log PD for high-frequency capability event streaming.

The full-featured structured log lives in the controller's BSS.

## IPC Endpoint

Agents write log entries by calling through the controller (which proxies to
the LogSvc library).  AUDIT-level entries are also streamed asynchronously
into the `audit_log` MR ring buffer accessible to the `cap_audit_log` PD.

The `cap_audit_log` PD opcodes (from `agentos.h`) are:

| Constant | Value | Description |
|----------|-------|-------------|
| `OP_CAP_LOG`       | 0x50 | Log a grant/revoke event |
| `OP_CAP_LOG_STATUS`| 0x51 | Query ring buffer status |
| `OP_CAP_LOG_DUMP`  | 0x52 | Read entries from ring |
| `OP_CAP_ATTEST`    | 0x53 | Generate signed attestation report |

The LogSvc IPC opcodes defined in this contract are used for the higher-level
structured log interface:

## Operations

| Opcode | Value | Description |
|--------|-------|-------------|
| `LOGSVC_OP_WRITE`     | 0x300 | Append a log entry |
| `LOGSVC_OP_QUERY`     | 0x301 | Query entries by seq/level/agent |
| `LOGSVC_OP_SET_LEVEL` | 0x302 | Change minimum log level |
| `LOGSVC_OP_STATUS`    | 0x303 | Ring buffer statistics |
| `LOGSVC_OP_HEALTH`    | 0x304 | Liveness probe |

## Log Levels

| Constant | Value | When to use |
|----------|-------|-------------|
| `LOGSVC_LEVEL_TRACE` | 0 | Verbose per-instruction tracing |
| `LOGSVC_LEVEL_DEBUG` | 1 | Development/debugging info |
| `LOGSVC_LEVEL_INFO`  | 2 | Normal operational events |
| `LOGSVC_LEVEL_WARN`  | 3 | Recoverable anomalies |
| `LOGSVC_LEVEL_ERROR` | 4 | Non-fatal errors |
| `LOGSVC_LEVEL_FATAL` | 5 | Unrecoverable errors |
| `LOGSVC_LEVEL_AUDIT` | 6 | Capability grant/revoke (always stored) |
| `LOGSVC_LEVEL_EVENT` | 7 | System lifecycle events |

## Error Codes

| Code | Value | Meaning |
|------|-------|---------|
| `LOGSVC_ERR_OK`          | 0  | Success |
| `LOGSVC_ERR_INVALID_ARG` | 1  | Bad level or null message |
| `LOGSVC_ERR_DENIED`      | 2  | Caller lacks audit-read capability |
| `LOGSVC_ERR_INTERNAL`    | 99 | Unexpected server error |

## Source Files

- `services/logsvc/logsvc.c` — implementation (write, query, JSON export)
- `kernel/agentos-root-task/src/cap_audit_log.c` — capability event streaming
- `kernel/agentos-root-task/include/agentos.h` — OP_CAP_LOG_* constants
