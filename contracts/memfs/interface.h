/*
 * agentOS MemFS IPC Contract — interface.h
 *
 * Formal seL4 IPC API contract for the MemFS virtual filesystem service.
 * MemFS is an in-memory filesystem with per-agent namespaces and semantic
 * tagging.  It is the reference storage service; agents may vibe-code a
 * replacement (e.g. the storage.v1 service which provides identical opcodes).
 *
 * The storage.v1 ABI (in services/abi/agentos_service_abi.h) reuses the
 * same STORAGE_OP_* opcode space; MemFS implements those same semantics.
 *
 * IPC mechanism: seL4_Call / seL4_Reply.
 * MR0 is the opcode on request; MR0 is the status on reply.
 * File paths and data pass through seL4 message registers for small payloads;
 * larger data uses a shared memory region (shmem offset/length pair).
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Version ─────────────────────────────────────────────────────────────── */

#define MEMFS_INTERFACE_VERSION  1

/* ── Limits ──────────────────────────────────────────────────────────────── */

#define MEMFS_MAX_FILES         64
#define MEMFS_MAX_FILE_SIZE     4096     /* 4KB per file */
#define MEMFS_PATH_MAX          256
#define MEMFS_TAG_MAX           64
#define MEMFS_MAX_TAGS          4        /* tags per file */

/* ── File flags ──────────────────────────────────────────────────────────── */

#define MEMFS_FLAG_READONLY     (1u << 0)   /* file cannot be overwritten */
#define MEMFS_FLAG_HIDDEN       (1u << 1)   /* excluded from LIST results */
#define MEMFS_FLAG_SYSTEM       (1u << 7)   /* created by init; owner-only ops */

/* ── Opcodes ─────────────────────────────────────────────────────────────── */

/* Primary storage ops (match STORAGE_OP_* in agentos_service_abi.h) */
#define MEMFS_OP_WRITE          0x30u   /* create or overwrite */
#define MEMFS_OP_READ           0x31u   /* read file contents */
#define MEMFS_OP_DELETE         0x32u   /* delete file */
#define MEMFS_OP_STAT           0x33u   /* get file metadata */
#define MEMFS_OP_LIST           0x34u   /* list files by prefix */
#define MEMFS_OP_STAT_SVC       0x20u   /* service-level statistics */

/* Extended ops */
#define MEMFS_OP_TAG            0x35u   /* attach a semantic tag */
#define MEMFS_OP_FIND_BY_TAG    0x36u   /* list files with a given tag */
#define MEMFS_OP_HEALTH         0x37u   /* liveness probe */

/* ── Error codes ─────────────────────────────────────────────────────────── */

#define MEMFS_ERR_OK            0u
#define MEMFS_ERR_INVALID_ARG   1u   /* null path, empty data, etc. */
#define MEMFS_ERR_NOT_FOUND     2u   /* file does not exist */
#define MEMFS_ERR_EXISTS        3u   /* file already exists (for create-only) */
#define MEMFS_ERR_TOO_BIG       4u   /* data exceeds MEMFS_MAX_FILE_SIZE */
#define MEMFS_ERR_NOMEM         5u   /* no free file slots */
#define MEMFS_ERR_DENIED        6u   /* caller does not own the file */
#define MEMFS_ERR_TOO_MANY_TAGS 7u   /* already at MEMFS_MAX_TAGS limit */
#define MEMFS_ERR_INTERNAL      99u

/* ── File stat structure (returned by MEMFS_OP_STAT) ────────────────────── */

typedef struct memfs_stat {
    uint32_t size;                      /* file size in bytes */
    uint32_t flags;                     /* MEMFS_FLAG_* bitmask */
    uint64_t created_at;                /* monotonic microseconds */
    uint64_t modified_at;
    uint32_t tag_count;                 /* number of attached tags */
    uint8_t  owner[32];                 /* AgentID of file owner */
} __attribute__((packed)) memfs_stat_t;

/* ── Request / reply structs ─────────────────────────────────────────────── */

/*
 * MEMFS_OP_WRITE
 *
 * Create or overwrite a file.  data_shmem_offset and data_len describe
 * the source data in the caller's shared memory region.
 * Returns MEMFS_ERR_TOO_BIG if data_len > MEMFS_MAX_FILE_SIZE.
 *
 * Request:  opcode, flags, data_shmem_offset, data_len, path
 * Reply:    status
 */
typedef struct memfs_write_req {
    uint32_t opcode;                    /* MEMFS_OP_WRITE */
    uint32_t flags;                     /* MEMFS_FLAG_* */
    uint32_t data_shmem_offset;
    uint32_t data_len;
    char     path[MEMFS_PATH_MAX];
} __attribute__((packed)) memfs_write_req_t;

typedef struct memfs_write_rep {
    uint32_t status;                    /* MEMFS_ERR_* */
} __attribute__((packed)) memfs_write_rep_t;

/*
 * MEMFS_OP_READ
 *
 * Read a file into the caller's shared memory region.
 * buf_shmem_offset is the destination; buf_len is the capacity.
 * On success bytes_read contains the actual bytes written.
 *
 * Request:  opcode, buf_shmem_offset, buf_len, path
 * Reply:    status, bytes_read
 */
typedef struct memfs_read_req {
    uint32_t opcode;                    /* MEMFS_OP_READ */
    uint32_t buf_shmem_offset;
    uint32_t buf_len;
    uint32_t _pad;
    char     path[MEMFS_PATH_MAX];
} __attribute__((packed)) memfs_read_req_t;

typedef struct memfs_read_rep {
    uint32_t status;
    uint32_t bytes_read;
} __attribute__((packed)) memfs_read_rep_t;

/*
 * MEMFS_OP_DELETE
 *
 * Delete a file.  Only the file's owner or init may delete.
 *
 * Request:  opcode, path
 * Reply:    status
 */
typedef struct memfs_delete_req {
    uint32_t opcode;                    /* MEMFS_OP_DELETE */
    uint32_t _pad;
    char     path[MEMFS_PATH_MAX];
} __attribute__((packed)) memfs_delete_req_t;

typedef struct memfs_delete_rep {
    uint32_t status;
} __attribute__((packed)) memfs_delete_rep_t;

/*
 * MEMFS_OP_STAT
 *
 * Retrieve file metadata.  The memfs_stat_t is written into the caller's
 * shmem MR at stat_shmem_offset.
 *
 * Request:  opcode, stat_shmem_offset, path
 * Reply:    status
 */
typedef struct memfs_stat_req {
    uint32_t opcode;                    /* MEMFS_OP_STAT */
    uint32_t stat_shmem_offset;
    char     path[MEMFS_PATH_MAX];
} __attribute__((packed)) memfs_stat_req_t;

typedef struct memfs_stat_rep {
    uint32_t status;
} __attribute__((packed)) memfs_stat_rep_t;

/*
 * MEMFS_OP_LIST
 *
 * List files whose path starts with prefix.
 * Writes up to max_entries NUL-terminated path strings into the caller's
 * shmem MR at buf_shmem_offset (packed back-to-back).
 *
 * Request:  opcode, max_entries, buf_shmem_offset, buf_len, prefix
 * Reply:    status, entry_count, bytes_written
 */
typedef struct memfs_list_req {
    uint32_t opcode;                    /* MEMFS_OP_LIST */
    uint32_t max_entries;
    uint32_t buf_shmem_offset;
    uint32_t buf_len;
    char     prefix[MEMFS_PATH_MAX];
} __attribute__((packed)) memfs_list_req_t;

typedef struct memfs_list_rep {
    uint32_t status;
    uint32_t entry_count;
    uint32_t bytes_written;
} __attribute__((packed)) memfs_list_rep_t;

/*
 * MEMFS_OP_STAT_SVC
 *
 * Service-level statistics: total file count and byte usage.
 *
 * Request:  opcode
 * Reply:    status, file_count, total_bytes
 */
typedef struct memfs_stat_svc_req {
    uint32_t opcode;                    /* MEMFS_OP_STAT_SVC */
} __attribute__((packed)) memfs_stat_svc_req_t;

typedef struct memfs_stat_svc_rep {
    uint32_t status;
    uint32_t file_count;
    uint32_t total_bytes;
} __attribute__((packed)) memfs_stat_svc_rep_t;

/*
 * MEMFS_OP_TAG
 *
 * Attach a semantic tag to a file.
 * The caller must own the file.  Returns MEMFS_ERR_TOO_MANY_TAGS if
 * the file already has MEMFS_MAX_TAGS tags.
 *
 * Request:  opcode, owner (32 bytes), path, tag
 * Reply:    status
 */
typedef struct memfs_tag_req {
    uint32_t opcode;                    /* MEMFS_OP_TAG */
    uint32_t _pad;
    uint8_t  owner[32];
    char     path[MEMFS_PATH_MAX];
    char     tag[MEMFS_TAG_MAX];
} __attribute__((packed)) memfs_tag_req_t;

typedef struct memfs_tag_rep {
    uint32_t status;
} __attribute__((packed)) memfs_tag_rep_t;

/*
 * MEMFS_OP_HEALTH
 *
 * Liveness probe.
 *
 * Request:  opcode
 * Reply:    status, file_count, version
 */
typedef struct memfs_health_req {
    uint32_t opcode;                    /* MEMFS_OP_HEALTH */
} __attribute__((packed)) memfs_health_req_t;

typedef struct memfs_health_rep {
    uint32_t status;
    uint32_t file_count;
    uint32_t version;                   /* MEMFS_INTERFACE_VERSION */
} __attribute__((packed)) memfs_health_rep_t;
