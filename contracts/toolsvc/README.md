# ToolSvc — Tool Registry and Dispatch Service Contract

## Overview

ToolSvc is the central registry and dispatcher for all callable tools in
agentOS.  It provides:

- Tool registration by any agent (tools are named, described, and schema-typed)
- MCP-compatible tool discovery (JSON format matching Model Context Protocol)
- Capability-gated invocation: callers must hold `CAPSTORE_CAP_TOOL`
- Routing of invocations to the provider agent via its seL4 badge
- Per-tool usage statistics (call count, latency)

The registry can hold up to 512 tools simultaneously.  Each tool has:
an owner AgentID, a JSON input schema, a JSON output schema, and a
routing badge for the provider PD.

## Protection Domain

ToolSvc is currently implemented as a library linked into the `controller` PD.
It surfaces through the `tools.registry` system channel on MsgBus and through
the userspace `tool-registry` server (`userspace/servers/tool-registry/`).

The Rust userspace server in `userspace/servers/tool-registry/src/lib.rs`
mirrors this contract for the higher-level agent runtime.

## IPC Endpoint

Agents reach ToolSvc via the controller PD.  The controller dispatches
opcode 0x400–0x406 to the internal ToolSvc library.

## Operations

| Opcode | Value | Description |
|--------|-------|-------------|
| `TOOLSVC_OP_REGISTER`   | 0x400 | Register a tool with name/schema/provider |
| `TOOLSVC_OP_UNREGISTER` | 0x401 | Remove a tool (provider only) |
| `TOOLSVC_OP_INVOKE`     | 0x402 | Invoke a tool by name |
| `TOOLSVC_OP_LIST`       | 0x403 | List all tools in MCP JSON format |
| `TOOLSVC_OP_INFO`       | 0x404 | Fetch metadata for one tool |
| `TOOLSVC_OP_STATS`      | 0x405 | Per-tool usage statistics |
| `TOOLSVC_OP_HEALTH`     | 0x406 | Liveness probe |

## Error Codes

| Code | Value | Meaning |
|------|-------|---------|
| `TOOLSVC_ERR_OK`           | 0  | Success |
| `TOOLSVC_ERR_INVALID_ARG`  | 1  | Bad opcode or name too long |
| `TOOLSVC_ERR_NOT_FOUND`    | 2  | Tool not registered |
| `TOOLSVC_ERR_EXISTS`       | 3  | Tool already registered by provider |
| `TOOLSVC_ERR_DENIED`       | 4  | Caller lacks CAPSTORE_CAP_TOOL |
| `TOOLSVC_ERR_NOMEM`        | 5  | Tool table full (512 max) |
| `TOOLSVC_ERR_PROVIDER_DOWN`| 6  | Provider agent not responding |
| `TOOLSVC_ERR_INTERNAL`     | 99 | Unexpected server error |

## MCP Compatibility

The `TOOLSVC_OP_LIST` response format is JSON-compatible with MCP tool
listings:

```json
{
  "tools": [
    {
      "name": "tool-name",
      "description": "...",
      "inputSchema": { ... },
      "calls": 42
    }
  ]
}
```

Agent LLMs can call `TOOLSVC_OP_LIST` to discover available tools and
`TOOLSVC_OP_INVOKE` to execute them without any adaptation layer.

## Source Files

- `services/toolsvc/toolsvc.c` — C implementation
- `userspace/servers/tool-registry/src/lib.rs` — Rust userspace server
