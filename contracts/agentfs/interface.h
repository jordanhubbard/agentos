/*
 * agentOS AgentFS IPC Contract — interface.h
 *
 * Formal seL4 IPC API contract for the AgentFS protection domain.
 * AgentFS is the agent-native persistent object store.  It is NOT POSIX.
 * Every item is an Object: content-addressed (BLAKE3), versioned,
 * capability-gated, metadata-rich, schema-typed, and event-emitting.
 *
 * Object model:
 *   - ObjectId: 32-byte identifier (BLAKE3 hash for blobs, UUID for mutable)
 *   - Every object: id, schema_type, version, size, cap_tag, metadata[]
 *   - Write-once blob store (immutable by content hash)
 *   - Mutable objects produce new versions (append-only log)
 *   - Optional vector embedding for semantic similarity queries
 *
 * Storage tiers:
 *   - Hot: shared MR (agentfs_store, 2MB)
 *   - Cold: deferred to external store via ModelSvc / HTTP
 *
 * IPC mechanism: seL4_Call / seL4_Reply (passive PD, priority 150).
 * MR0 carries the opcode on request; MR0 carries the status on reply.
 * Object data passes through the agentfs_store shared MR.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Version ─────────────────────────────────────────────────────────────── */

#define AGENTFS_INTERFACE_VERSION   1

/* ── Limits ──────────────────────────────────────────────────────────────── */

#define AGENTFS_OBJECT_ID_BYTES     32     /* BLAKE3/UUID identifier */
#define AGENTFS_SCHEMA_MAX          32     /* e.g. "agentOS::InferenceResult" */
#define AGENTFS_MAX_HOT_OBJECTS     256
#define AGENTFS_HOT_STORE_SIZE      (256 * 1024)  /* 256KB hot tier */
#define AGENTFS_MAX_VECTOR_DIM      512

/* ── Object state flags ──────────────────────────────────────────────────── */

#define AGENTFS_STATE_LIVE          0u
#define AGENTFS_STATE_TOMBSTONE     1u    /* soft-deleted */
#define AGENTFS_STATE_EVICTED       2u    /* moved to cold tier */

/* ── Query filter flags ──────────────────────────────────────────────────── */

#define AGENTFS_QUERY_LIVE_ONLY     (1u << 0)  /* skip tombstones */
#define AGENTFS_QUERY_BY_SCHEMA     (1u << 1)  /* filter by schema_type */
#define AGENTFS_QUERY_BY_CAP_TAG    (1u << 2)  /* filter by cap_tag */

/* ── Opcodes ─────────────────────────────────────────────────────────────── */

#define AGENTFS_OP_PUT              0x30u   /* write / create object */
#define AGENTFS_OP_GET              0x31u   /* read object by ID */
#define AGENTFS_OP_QUERY            0x32u   /* list/filter objects */
#define AGENTFS_OP_DELETE           0x33u   /* soft delete (tombstone) */
#define AGENTFS_OP_VECTOR           0x34u   /* vector similarity query */
#define AGENTFS_OP_STAT             0x35u   /* get object metadata */
#define AGENTFS_OP_HEALTH           0x36u   /* liveness probe */

/* ── Error codes ─────────────────────────────────────────────────────────── */

#define AGENTFS_ERR_OK              0u
#define AGENTFS_ERR_NO_CAP          1u   /* caller lacks cap_tag for this object */
#define AGENTFS_ERR_NOT_FOUND       2u   /* object not found */
#define AGENTFS_ERR_NO_SPACE        3u   /* hot store full */
#define AGENTFS_ERR_TYPE_MISMATCH   4u   /* schema_type conflict */
#define AGENTFS_ERR_INVALID_ARG     5u   /* null pointer, zero size, etc. */
#define AGENTFS_ERR_INTERNAL        99u

/* ── Object ID ───────────────────────────────────────────────────────────── */

typedef struct agentfs_object_id {
    uint8_t  bytes[AGENTFS_OBJECT_ID_BYTES];
    uint8_t  scheme;   /* 0=null, 1=blake3, 2=uuid */
    uint8_t  _pad[3];
} __attribute__((packed)) agentfs_object_id_t;

/* ── Object metadata (returned by AGENTFS_OP_STAT) ───────────────────────── */

typedef struct agentfs_object_stat {
    agentfs_object_id_t id;
    char     schema[AGENTFS_SCHEMA_MAX];
    uint32_t version;
    uint32_t size;
    uint32_t cap_tag;                   /* badge required to read */
    uint32_t state;                     /* AGENTFS_STATE_* */
    uint64_t created_at;                /* monotonic microseconds */
    uint64_t modified_at;
    uint16_t vec_dim;                   /* 0 if no vector embedding */
    uint16_t _pad;
} __attribute__((packed)) agentfs_object_stat_t;

/* ── Request / reply structs ─────────────────────────────────────────────── */

/*
 * AGENTFS_OP_PUT
 *
 * Write (create or update) an object.  For blob objects (scheme=blake3),
 * the ObjectId is computed server-side and returned.  For mutable objects
 * (scheme=uuid), the caller supplies the ID and a new version is created.
 *
 * Object data is read from the caller's MR share of agentfs_store at
 * data_store_offset.
 *
 * Request:  opcode, cap_tag, data_store_offset, data_len, id_hint (optional),
 *           schema
 * Reply:    status, assigned_id (full agentfs_object_id_t)
 */
typedef struct agentfs_put_req {
    uint32_t opcode;                        /* AGENTFS_OP_PUT */
    uint32_t cap_tag;                       /* capability badge required to read */
    uint32_t data_store_offset;             /* offset into agentfs_store MR */
    uint32_t data_len;
    agentfs_object_id_t id_hint;            /* for mutable objects; zero for blobs */
    char     schema[AGENTFS_SCHEMA_MAX];
} __attribute__((packed)) agentfs_put_req_t;

typedef struct agentfs_put_rep {
    uint32_t status;                        /* AGENTFS_ERR_* */
    uint32_t _pad;
    agentfs_object_id_t id;                 /* assigned ObjectId */
} __attribute__((packed)) agentfs_put_rep_t;

/*
 * AGENTFS_OP_GET
 *
 * Read an object by ID.  The caller must hold a capability whose badge
 * matches the object's cap_tag.  Object data is written to the caller's
 * MR share of agentfs_store at buf_store_offset.
 *
 * Request:  opcode, caller_badge, buf_store_offset, buf_len, id
 * Reply:    status, bytes_written, version
 */
typedef struct agentfs_get_req {
    uint32_t opcode;                        /* AGENTFS_OP_GET */
    uint32_t caller_badge;                  /* must match object cap_tag */
    uint32_t buf_store_offset;
    uint32_t buf_len;
    agentfs_object_id_t id;
} __attribute__((packed)) agentfs_get_req_t;

typedef struct agentfs_get_rep {
    uint32_t status;
    uint32_t version;
    uint32_t bytes_written;
} __attribute__((packed)) agentfs_get_rep_t;

/*
 * AGENTFS_OP_QUERY
 *
 * List/filter objects.  Results are written as an array of agentfs_object_id_t
 * into the caller's agentfs_store MR at buf_store_offset.
 *
 * query_flags: AGENTFS_QUERY_* bitmask
 * schema_filter: used when AGENTFS_QUERY_BY_SCHEMA is set
 * cap_tag_filter: used when AGENTFS_QUERY_BY_CAP_TAG is set
 *
 * Request:  opcode, query_flags, cap_tag_filter, max_results, buf_store_offset,
 *           buf_len, schema_filter
 * Reply:    status, result_count, bytes_written
 */
typedef struct agentfs_query_req {
    uint32_t opcode;                        /* AGENTFS_OP_QUERY */
    uint32_t query_flags;                   /* AGENTFS_QUERY_* */
    uint32_t cap_tag_filter;
    uint32_t max_results;
    uint32_t buf_store_offset;
    uint32_t buf_len;
    char     schema_filter[AGENTFS_SCHEMA_MAX];
} __attribute__((packed)) agentfs_query_req_t;

typedef struct agentfs_query_rep {
    uint32_t status;
    uint32_t result_count;
    uint32_t bytes_written;
} __attribute__((packed)) agentfs_query_rep_t;

/*
 * AGENTFS_OP_DELETE
 *
 * Soft-delete an object (creates a tombstone).  The caller must hold
 * a capability matching the object's cap_tag.
 *
 * Request:  opcode, caller_badge, id
 * Reply:    status
 */
typedef struct agentfs_delete_req {
    uint32_t opcode;                        /* AGENTFS_OP_DELETE */
    uint32_t caller_badge;
    agentfs_object_id_t id;
} __attribute__((packed)) agentfs_delete_req_t;

typedef struct agentfs_delete_rep {
    uint32_t status;
} __attribute__((packed)) agentfs_delete_rep_t;

/*
 * AGENTFS_OP_VECTOR
 *
 * Vector similarity search.  The query vector is read from the caller's
 * agentfs_store MR at vec_store_offset (array of float32, vec_dim elements).
 * Top-k nearest neighbors are returned as agentfs_object_id_t[] written to
 * result_store_offset.
 *
 * Request:  opcode, vec_dim, top_k, vec_store_offset, result_store_offset,
 *           result_buf_len, cap_tag_filter (0 = any)
 * Reply:    status, result_count
 */
typedef struct agentfs_vector_req {
    uint32_t opcode;                        /* AGENTFS_OP_VECTOR */
    uint32_t vec_dim;                       /* dimensionality (max 512) */
    uint32_t top_k;                         /* number of neighbors wanted */
    uint32_t vec_store_offset;              /* float32[vec_dim] in store MR */
    uint32_t result_store_offset;
    uint32_t result_buf_len;
    uint32_t cap_tag_filter;                /* 0 = no filter */
} __attribute__((packed)) agentfs_vector_req_t;

typedef struct agentfs_vector_rep {
    uint32_t status;
    uint32_t result_count;
} __attribute__((packed)) agentfs_vector_rep_t;

/*
 * AGENTFS_OP_STAT
 *
 * Fetch full metadata for an object.  The agentfs_object_stat_t is written
 * into the caller's agentfs_store MR at stat_store_offset.
 *
 * Request:  opcode, stat_store_offset, caller_badge, id
 * Reply:    status
 */
typedef struct agentfs_stat_req {
    uint32_t opcode;                        /* AGENTFS_OP_STAT */
    uint32_t stat_store_offset;
    uint32_t caller_badge;
    uint32_t _pad;
    agentfs_object_id_t id;
} __attribute__((packed)) agentfs_stat_req_t;

typedef struct agentfs_stat_rep {
    uint32_t status;
} __attribute__((packed)) agentfs_stat_rep_t;

/*
 * AGENTFS_OP_HEALTH
 *
 * Liveness probe.
 *
 * Request:  opcode
 * Reply:    status, hot_object_count, hot_bytes_used, version
 */
typedef struct agentfs_health_req {
    uint32_t opcode;                        /* AGENTFS_OP_HEALTH */
} __attribute__((packed)) agentfs_health_req_t;

typedef struct agentfs_health_rep {
    uint32_t status;
    uint32_t hot_object_count;
    uint32_t hot_bytes_used;
    uint32_t version;                       /* AGENTFS_INTERFACE_VERSION */
} __attribute__((packed)) agentfs_health_rep_t;

/* ── Blob store opcodes (for VOS snapshot / restore path) ────────────────── */

/*
 * AGENTFS_OP_WRITE (0xAF01)
 *
 * Write a raw byte blob under a caller-supplied key string.  The blob data is
 * placed in the agentfs_store shared MR at data_store_offset.  On success the
 * service returns a 64-bit storage token (token_lo + token_hi) that can later
 * be passed to AGENTFS_OP_READ to retrieve the blob.
 *
 * The token is formed from the first 8 bytes of the assigned ObjectId:
 *   token_lo = bytes[0..3] as big-endian uint32
 *   token_hi = bytes[4..7] as big-endian uint32
 *
 * Request MRs:
 *   MR0 = AGENTFS_OP_WRITE
 *   MR1 = data_store_offset (offset into agentfs_store MR)
 *   MR2 = data_len (bytes)
 *   MR3 = key_len  (length of key string at key_store_offset)
 *   MR4 = key_store_offset (key string in agentfs_store MR)
 *
 * Reply MRs on success:
 *   MR0 = AGENTFS_ERR_OK
 *   MR1 = token_lo
 *   MR2 = token_hi
 *
 * Errors: AGENTFS_ERR_NO_SPACE, AGENTFS_ERR_INVALID_ARG, AGENTFS_ERR_INTERNAL
 */
#define AGENTFS_OP_WRITE    0xAF01u

typedef struct agentfs_write_req {
    uint32_t opcode;             /* AGENTFS_OP_WRITE */
    uint32_t data_store_offset;  /* offset into agentfs_store MR */
    uint32_t data_len;
    uint32_t key_len;
    uint32_t key_store_offset;
    uint32_t _pad;
} __attribute__((packed)) agentfs_write_req_t;

typedef struct agentfs_write_rep {
    uint32_t status;   /* AGENTFS_ERR_* */
    uint32_t token_lo; /* low  32 bits of storage token */
    uint32_t token_hi; /* high 32 bits of storage token */
    uint32_t _pad;
} __attribute__((packed)) agentfs_write_rep_t;

/*
 * AGENTFS_OP_READ (0xAF02)
 *
 * Read a blob previously stored with AGENTFS_OP_WRITE.  The blob is written
 * into the agentfs_store MR at buf_store_offset.  The caller supplies the
 * token obtained from the corresponding AGENTFS_OP_WRITE reply.
 *
 * Request MRs:
 *   MR0 = AGENTFS_OP_READ
 *   MR1 = token_lo
 *   MR2 = token_hi
 *   MR3 = buf_store_offset (destination in agentfs_store MR)
 *   MR4 = buf_len (maximum bytes to write)
 *
 * Reply MRs on success:
 *   MR0 = AGENTFS_ERR_OK
 *   MR1 = bytes_written
 *
 * Errors: AGENTFS_ERR_NOT_FOUND, AGENTFS_ERR_INVALID_ARG, AGENTFS_ERR_INTERNAL
 */
#define AGENTFS_OP_READ     0xAF02u

typedef struct agentfs_read_req {
    uint32_t opcode;            /* AGENTFS_OP_READ */
    uint32_t token_lo;
    uint32_t token_hi;
    uint32_t buf_store_offset;
    uint32_t buf_len;
    uint32_t _pad;
} __attribute__((packed)) agentfs_read_req_t;

typedef struct agentfs_read_rep {
    uint32_t status;        /* AGENTFS_ERR_* */
    uint32_t bytes_written;
    uint32_t _pad[2];
} __attribute__((packed)) agentfs_read_rep_t;
