/*
 * AgentFS IPC Contract
 *
 * AgentFS is the distributed object store and service registry for agentOS.
 * It stores WASM modules, configuration blobs, and ephemeral agent state.
 * It also serves as the /devices namespace for guest OS device discovery.
 *
 * Channel: CH_VFS_SERVER (reused for AgentFS in current build)
 * Opcodes: MSG_AGENTFS_* (see agentos.h)
 *
 * Invariants:
 *   - All path arguments are NUL-terminated strings placed in the shared
 *     agentfs_shmem region before the IPC call.
 *   - READ/WRITE data is also transferred via agentfs_shmem.
 *   - SEARCH queries use a prefix-match on the path component.
 *   - A successful WRITE publishes EVT_OBJECT_CREATED to the EventBus.
 *   - DELETE tombstones the object; EVT_OBJECT_DELETED is published.
 *   - STAT on a non-existent path returns AGENTFS_ERR_NOT_FOUND.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define AGENTFS_CH_CONTROLLER   CH_VFS_SERVER   /* controller → agentfs */

/* ─── Request structs ────────────────────────────────────────────────────── */

struct agentfs_req_read {
    uint32_t inode;             /* inode from a prior STAT call */
    uint32_t offset;            /* byte offset into object */
    uint32_t len;               /* bytes to read */
};

struct agentfs_req_write {
    uint32_t inode;             /* 0 = create new object at path in shmem */
    uint32_t offset;
    uint32_t len;
};

struct agentfs_req_stat {
    /* NUL-terminated path string placed in shmem before call */
    uint32_t path_len;          /* byte length of path (including NUL) */
};

struct agentfs_req_list {
    uint32_t path_len;          /* directory path in shmem */
    uint32_t max_entries;       /* max entries to return */
};

struct agentfs_req_delete {
    uint32_t path_len;
};

struct agentfs_req_search {
    uint32_t query_len;         /* prefix query string in shmem */
    uint32_t max_results;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct agentfs_reply_read {
    uint32_t ok;
    uint32_t actual;            /* bytes actually read */
};

struct agentfs_reply_write {
    uint32_t ok;
    uint32_t inode;             /* inode of written object */
    uint32_t written;           /* bytes written */
};

struct agentfs_reply_stat {
    uint32_t ok;
    uint32_t inode;
    uint32_t size_lo;           /* object size (low 32 bits) */
    uint32_t size_hi;
    uint32_t flags;             /* AGENTFS_FLAG_* */
};

#define AGENTFS_FLAG_DIR      (1u << 0)
#define AGENTFS_FLAG_WASM     (1u << 1)
#define AGENTFS_FLAG_READONLY (1u << 2)
#define AGENTFS_FLAG_EVICTED  (1u << 3)  /* in cold tier */

struct agentfs_reply_list {
    uint32_t ok;
    uint32_t count;             /* entries written to shmem */
};

struct agentfs_reply_delete {
    uint32_t ok;
};

struct agentfs_reply_search {
    uint32_t ok;
    uint32_t count;             /* results written to shmem */
};

/* ─── Shmem layout: directory listing entry ──────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint32_t size_lo;
    uint32_t flags;
    uint8_t  name[64];          /* NUL-terminated object name */
} agentfs_dirent_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum agentfs_error {
    AGENTFS_OK              = 0,
    AGENTFS_ERR_NOT_FOUND   = 1,
    AGENTFS_ERR_NO_SPACE    = 2,
    AGENTFS_ERR_BAD_PATH    = 3,
    AGENTFS_ERR_READONLY    = 4,
    AGENTFS_ERR_TOO_LARGE   = 5,
    AGENTFS_ERR_BAD_INODE   = 6,
};
