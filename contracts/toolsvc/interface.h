/*
 * agentOS ToolSvc IPC Contract — interface.h
 *
 * Formal seL4 IPC API contract for the ToolSvc tool registry and dispatch
 * service.  ToolSvc manages all callable tools across the agent mesh.
 * Agents register tools they provide; other agents invoke tools by name
 * through capability-gated dispatch.
 *
 * The interface is MCP-compatible (Model Context Protocol): the tool
 * registry entry format mirrors MCP tool definitions so agent LLMs can
 * natively discover and invoke tools without adaptation.
 *
 * IPC mechanism: seL4_Call / seL4_Reply.
 * MR0 carries the opcode on request; MR0 carries the status on reply.
 * Large fields (schemas, arguments) pass through a shared memory region.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Version ─────────────────────────────────────────────────────────────── */

#define TOOLSVC_INTERFACE_VERSION  1

/* ── Limits ──────────────────────────────────────────────────────────────── */

#define TOOLSVC_MAX_TOOLS           512
#define TOOLSVC_TOOL_NAME_MAX       128
#define TOOLSVC_TOOL_DESC_MAX       512
#define TOOLSVC_SCHEMA_MAX          2048   /* JSON schema per tool (in/out) */
#define TOOLSVC_AGENT_ID_BYTES      32

/* ── Tool flags ──────────────────────────────────────────────────────────── */

#define TOOLSVC_FLAG_SYSTEM         (1u << 0)  /* built-in; cannot be unregistered */
#define TOOLSVC_FLAG_ASYNC          (1u << 1)  /* non-blocking invocation supported */
#define TOOLSVC_FLAG_RESTRICTED     (1u << 2)  /* requires elevated capability */

/* ── Opcodes ─────────────────────────────────────────────────────────────── */

#define TOOLSVC_OP_REGISTER         0x400u  /* register a tool */
#define TOOLSVC_OP_UNREGISTER       0x401u  /* remove a tool */
#define TOOLSVC_OP_INVOKE           0x402u  /* invoke a tool by name */
#define TOOLSVC_OP_LIST             0x403u  /* list available tools (MCP format) */
#define TOOLSVC_OP_INFO             0x404u  /* fetch metadata for one tool */
#define TOOLSVC_OP_STATS            0x405u  /* per-tool usage statistics */
#define TOOLSVC_OP_HEALTH           0x406u  /* liveness probe */

/* ── Error codes ─────────────────────────────────────────────────────────── */

#define TOOLSVC_ERR_OK              0u
#define TOOLSVC_ERR_INVALID_ARG     1u   /* bad opcode, name too long, etc. */
#define TOOLSVC_ERR_NOT_FOUND       2u   /* tool name not registered */
#define TOOLSVC_ERR_EXISTS          3u   /* tool already registered by this provider */
#define TOOLSVC_ERR_DENIED          4u   /* caller lacks CAPSTORE_CAP_TOOL */
#define TOOLSVC_ERR_NOMEM           5u   /* tool table full */
#define TOOLSVC_ERR_PROVIDER_DOWN   6u   /* provider agent is not responding */
#define TOOLSVC_ERR_INTERNAL        99u

/* ── Tool metadata structure (returned by TOOLSVC_OP_INFO) ──────────────── */

typedef struct toolsvc_tool_info {
    char     name[TOOLSVC_TOOL_NAME_MAX];
    char     description[TOOLSVC_TOOL_DESC_MAX];
    uint8_t  provider[TOOLSVC_AGENT_ID_BYTES]; /* AgentID of provider */
    uint64_t provider_badge;                    /* seL4 badge for routing */
    uint32_t flags;                             /* TOOLSVC_FLAG_* */
    uint64_t call_count;
    uint64_t total_latency_us;
    uint64_t registered_at;                     /* monotonic microseconds */
} __attribute__((packed)) toolsvc_tool_info_t;

/* ── Request / reply structs ─────────────────────────────────────────────── */

/*
 * TOOLSVC_OP_REGISTER
 *
 * Register a tool provided by the calling agent.
 * input_schema and output_schema are JSON strings (MCP-compatible).
 * They are read from the caller's shmem MR at the given offsets.
 *
 * Request:  opcode, flags, provider_badge, input_schema_offset,
 *           input_schema_len, output_schema_offset, output_schema_len,
 *           provider (32 bytes), name, description
 * Reply:    status
 */
typedef struct toolsvc_register_req {
    uint32_t opcode;                            /* TOOLSVC_OP_REGISTER */
    uint32_t flags;                             /* TOOLSVC_FLAG_* */
    uint64_t provider_badge;
    uint32_t input_schema_shmem_offset;
    uint32_t input_schema_len;
    uint32_t output_schema_shmem_offset;
    uint32_t output_schema_len;
    uint8_t  provider[TOOLSVC_AGENT_ID_BYTES];
    char     name[TOOLSVC_TOOL_NAME_MAX];
    char     description[TOOLSVC_TOOL_DESC_MAX];
} __attribute__((packed)) toolsvc_register_req_t;

typedef struct toolsvc_register_rep {
    uint32_t status;                            /* TOOLSVC_ERR_* */
} __attribute__((packed)) toolsvc_register_rep_t;

/*
 * TOOLSVC_OP_UNREGISTER
 *
 * Remove a tool.  The caller must be the tool's registered provider.
 * System tools (TOOLSVC_FLAG_SYSTEM) cannot be unregistered.
 *
 * Request:  opcode, provider (32 bytes), name
 * Reply:    status
 */
typedef struct toolsvc_unregister_req {
    uint32_t opcode;                            /* TOOLSVC_OP_UNREGISTER */
    uint32_t _pad;
    uint8_t  provider[TOOLSVC_AGENT_ID_BYTES];
    char     name[TOOLSVC_TOOL_NAME_MAX];
} __attribute__((packed)) toolsvc_unregister_req_t;

typedef struct toolsvc_unregister_rep {
    uint32_t status;
} __attribute__((packed)) toolsvc_unregister_rep_t;

/*
 * TOOLSVC_OP_INVOKE
 *
 * Invoke a registered tool.  ToolSvc validates the caller's CAPSTORE_CAP_TOOL
 * capability, then routes the invocation to the provider agent via seL4 IPC
 * using the provider's badge.  Blocks until the provider replies or timeout.
 *
 * input is read from the caller's shmem MR at input_shmem_offset.
 * output is written to the caller's shmem MR at output_shmem_offset.
 * timeout_ms: 0 = no timeout (block indefinitely).
 *
 * Request:  opcode, timeout_ms, input_shmem_offset, input_len,
 *           output_shmem_offset, output_buf_len,
 *           caller (32 bytes), name
 * Reply:    status, output_len, latency_us
 */
typedef struct toolsvc_invoke_req {
    uint32_t opcode;                            /* TOOLSVC_OP_INVOKE */
    uint32_t timeout_ms;
    uint32_t input_shmem_offset;
    uint32_t input_len;
    uint32_t output_shmem_offset;
    uint32_t output_buf_len;
    uint8_t  caller[TOOLSVC_AGENT_ID_BYTES];
    char     name[TOOLSVC_TOOL_NAME_MAX];
} __attribute__((packed)) toolsvc_invoke_req_t;

typedef struct toolsvc_invoke_rep {
    uint32_t status;
    uint32_t output_len;
    uint64_t latency_us;
} __attribute__((packed)) toolsvc_invoke_rep_t;

/*
 * TOOLSVC_OP_LIST
 *
 * Enumerate available tools in MCP-compatible JSON format.
 * The JSON string is written into the caller's shmem MR at buf_shmem_offset.
 * The format is: {"tools":[{"name":"...","description":"...","inputSchema":{...},"calls":N},...]}
 *
 * Request:  opcode, buf_shmem_offset, buf_len, caller (32 bytes)
 * Reply:    status, tool_count, bytes_written
 */
typedef struct toolsvc_list_req {
    uint32_t opcode;                            /* TOOLSVC_OP_LIST */
    uint32_t buf_shmem_offset;
    uint32_t buf_len;
    uint32_t _pad;
    uint8_t  caller[TOOLSVC_AGENT_ID_BYTES];
} __attribute__((packed)) toolsvc_list_req_t;

typedef struct toolsvc_list_rep {
    uint32_t status;
    uint32_t tool_count;
    uint32_t bytes_written;
} __attribute__((packed)) toolsvc_list_rep_t;

/*
 * TOOLSVC_OP_INFO
 *
 * Fetch full metadata for one tool.
 * The toolsvc_tool_info_t is written into the caller's shmem MR at
 * info_shmem_offset.
 *
 * Request:  opcode, info_shmem_offset, name
 * Reply:    status
 */
typedef struct toolsvc_info_req {
    uint32_t opcode;                            /* TOOLSVC_OP_INFO */
    uint32_t info_shmem_offset;
    char     name[TOOLSVC_TOOL_NAME_MAX];
} __attribute__((packed)) toolsvc_info_req_t;

typedef struct toolsvc_info_rep {
    uint32_t status;
} __attribute__((packed)) toolsvc_info_rep_t;

/*
 * TOOLSVC_OP_STATS
 *
 * Retrieve aggregated invocation statistics for a tool.
 *
 * Request:  opcode, name
 * Reply:    status, call_count, total_latency_us, avg_latency_us
 */
typedef struct toolsvc_stats_req {
    uint32_t opcode;                            /* TOOLSVC_OP_STATS */
    uint32_t _pad;
    char     name[TOOLSVC_TOOL_NAME_MAX];
} __attribute__((packed)) toolsvc_stats_req_t;

typedef struct toolsvc_stats_rep {
    uint32_t status;
    uint32_t _pad;
    uint64_t call_count;
    uint64_t total_latency_us;
    uint64_t avg_latency_us;
} __attribute__((packed)) toolsvc_stats_rep_t;

/*
 * TOOLSVC_OP_HEALTH
 *
 * Liveness probe.
 *
 * Request:  opcode
 * Reply:    status, tool_count, version
 */
typedef struct toolsvc_health_req {
    uint32_t opcode;                            /* TOOLSVC_OP_HEALTH */
} __attribute__((packed)) toolsvc_health_req_t;

typedef struct toolsvc_health_rep {
    uint32_t status;
    uint32_t tool_count;
    uint32_t version;                           /* TOOLSVC_INTERFACE_VERSION */
} __attribute__((packed)) toolsvc_health_rep_t;
