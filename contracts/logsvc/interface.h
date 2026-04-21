/*
 * agentOS LogSvc IPC Contract — interface.h
 *
 * Formal seL4 IPC API contract for the LogSvc audit and logging service.
 * LogSvc is the centralized, capability-audited logging sink for the entire
 * system.  It maintains a circular ring buffer of structured log entries and
 * exposes them for agent introspection.  Every capability operation, fault,
 * and agent lifecycle event is routed here.
 *
 * Log entries are structured (JSON-serializable) so agent LLMs can directly
 * read and reason about system history.
 *
 * IPC mechanism: seL4_Call / seL4_Reply.
 * MR0 is the opcode on request; MR0 is the status on reply.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Version ─────────────────────────────────────────────────────────────── */

#define LOGSVC_INTERFACE_VERSION  1

/* ── Limits ──────────────────────────────────────────────────────────────── */

#define LOGSVC_MAX_ENTRIES      16384   /* circular ring buffer depth */
#define LOGSVC_MSG_MAX          512     /* bytes per log message */
#define LOGSVC_COMPONENT_MAX    64      /* bytes for component name */
#define LOGSVC_AGENT_ID_BYTES   32      /* AgentID is 32 bytes */
#define LOGSVC_MAX_QUERY_BATCH  256     /* max entries returned per QUERY call */

/* ── Log levels ──────────────────────────────────────────────────────────── */

#define LOGSVC_LEVEL_TRACE      0u
#define LOGSVC_LEVEL_DEBUG      1u
#define LOGSVC_LEVEL_INFO       2u
#define LOGSVC_LEVEL_WARN       3u
#define LOGSVC_LEVEL_ERROR      4u
#define LOGSVC_LEVEL_FATAL      5u
#define LOGSVC_LEVEL_AUDIT      6u   /* capability operations; always recorded */
#define LOGSVC_LEVEL_EVENT      7u   /* system lifecycle events */

/* ── Opcodes ─────────────────────────────────────────────────────────────── */

#define LOGSVC_OP_WRITE         0x300u  /* write a log entry */
#define LOGSVC_OP_QUERY         0x301u  /* query entries by seq / level / agent */
#define LOGSVC_OP_SET_LEVEL     0x302u  /* change minimum log level */
#define LOGSVC_OP_STATUS        0x303u  /* ring buffer statistics */
#define LOGSVC_OP_HEALTH        0x304u  /* liveness probe */

/* ── Error codes ─────────────────────────────────────────────────────────── */

#define LOGSVC_ERR_OK           0u
#define LOGSVC_ERR_INVALID_ARG  1u   /* bad level, null message, etc. */
#define LOGSVC_ERR_DENIED       2u   /* caller lacks LOGSVC_CAP_AUDIT read cap */
#define LOGSVC_ERR_INTERNAL     99u

/* ── Packed log entry (returned in shmem by LOGSVC_OP_QUERY) ────────────── */

typedef struct logsvc_entry {
    uint64_t seq;                           /* monotonic sequence number */
    uint64_t timestamp_us;                  /* microseconds since boot */
    uint8_t  agent[LOGSVC_AGENT_ID_BYTES];  /* AgentID (zero = kernel) */
    uint32_t level;                         /* LOGSVC_LEVEL_* */
    uint32_t _pad;
    char     component[LOGSVC_COMPONENT_MAX];
    char     message[LOGSVC_MSG_MAX];
} __attribute__((packed)) logsvc_entry_t;

/* ── Request / reply structs ─────────────────────────────────────────────── */

/*
 * LOGSVC_OP_WRITE
 *
 * Append a log entry.  Entries below the current min_level are silently
 * dropped (AUDIT-level entries are always written).
 *
 * If message_len == 0 the server reads message as a NUL-terminated string
 * from the inline message[] field (max LOGSVC_MSG_MAX-1 bytes).  If
 * message_len > 0 the message is read from the caller's shmem MR at
 * msg_shmem_offset.
 *
 * Request:  opcode, level, msg_shmem_offset, message_len, agent (32 bytes),
 *           component, message (inline fallback when message_len == 0)
 * Reply:    status, seq
 */
typedef struct logsvc_write_req {
    uint32_t opcode;                        /* LOGSVC_OP_WRITE */
    uint32_t level;                         /* LOGSVC_LEVEL_* */
    uint32_t msg_shmem_offset;              /* 0 if using inline message */
    uint32_t message_len;                   /* 0 means use inline message[] */
    uint8_t  agent[LOGSVC_AGENT_ID_BYTES];
    char     component[LOGSVC_COMPONENT_MAX];
    char     message[LOGSVC_MSG_MAX];       /* used only when message_len == 0 */
} __attribute__((packed)) logsvc_write_req_t;

typedef struct logsvc_write_rep {
    uint32_t status;                        /* LOGSVC_ERR_* */
    uint32_t _pad;
    uint64_t seq;                           /* sequence number assigned */
} __attribute__((packed)) logsvc_write_rep_t;

/*
 * LOGSVC_OP_QUERY
 *
 * Retrieve log entries matching the given filters.
 * Results are written as an array of logsvc_entry_t into the caller's shmem
 * MR starting at buf_shmem_offset.
 *
 * since_seq:     only return entries with seq >= since_seq (use 0 for oldest)
 * filter_level:  minimum level to include (use LOGSVC_LEVEL_TRACE for all)
 * filter_agent:  if non-zero, restrict to entries from this AgentID
 * max_count:     max entries to return (capped at LOGSVC_MAX_QUERY_BATCH)
 *
 * Caller must hold a capability of type CAPSTORE_CAP_SELF or
 * CAPSTORE_CAP_STORE (or be the init task) to query entries from agents
 * other than themselves.
 *
 * Request:  opcode, filter_level, max_count, since_seq,
 *           filter_agent (32 bytes), buf_shmem_offset, buf_len
 * Reply:    status, entry_count, next_seq
 */
typedef struct logsvc_query_req {
    uint32_t opcode;                        /* LOGSVC_OP_QUERY */
    uint32_t filter_level;                  /* LOGSVC_LEVEL_* minimum */
    uint32_t max_count;
    uint32_t buf_shmem_offset;
    uint32_t buf_len;
    uint32_t _pad;
    uint64_t since_seq;
    uint8_t  filter_agent[LOGSVC_AGENT_ID_BYTES]; /* all zeros = no filter */
} __attribute__((packed)) logsvc_query_req_t;

typedef struct logsvc_query_rep {
    uint32_t status;
    uint32_t entry_count;
    uint64_t next_seq;                      /* use as since_seq for pagination */
} __attribute__((packed)) logsvc_query_rep_t;

/*
 * LOGSVC_OP_SET_LEVEL
 *
 * Change the minimum log level.  AUDIT and EVENT entries are always written
 * regardless of this setting.  Only the init task or controller may call this.
 *
 * Request:  opcode, min_level
 * Reply:    status
 */
typedef struct logsvc_set_level_req {
    uint32_t opcode;                        /* LOGSVC_OP_SET_LEVEL */
    uint32_t min_level;                     /* LOGSVC_LEVEL_* */
} __attribute__((packed)) logsvc_set_level_req_t;

typedef struct logsvc_set_level_rep {
    uint32_t status;
} __attribute__((packed)) logsvc_set_level_rep_t;

/*
 * LOGSVC_OP_STATUS
 *
 * Return ring buffer statistics.
 *
 * Request:  opcode
 * Reply:    status, total_entries, head_seq, dropped_count, min_level
 */
typedef struct logsvc_status_req {
    uint32_t opcode;                        /* LOGSVC_OP_STATUS */
} __attribute__((packed)) logsvc_status_req_t;

typedef struct logsvc_status_rep {
    uint32_t status;
    uint32_t min_level;                     /* current minimum log level */
    uint64_t total_entries;                 /* total entries ever written */
    uint64_t head_seq;                      /* next sequence number to assign */
    uint64_t dropped_count;                 /* entries dropped (below min_level) */
} __attribute__((packed)) logsvc_status_rep_t;

/*
 * LOGSVC_OP_HEALTH
 *
 * Liveness probe.
 *
 * Request:  opcode
 * Reply:    status, version
 */
typedef struct logsvc_health_req {
    uint32_t opcode;                        /* LOGSVC_OP_HEALTH */
} __attribute__((packed)) logsvc_health_req_t;

typedef struct logsvc_health_rep {
    uint32_t status;
    uint32_t version;                       /* LOGSVC_INTERFACE_VERSION */
} __attribute__((packed)) logsvc_health_rep_t;
