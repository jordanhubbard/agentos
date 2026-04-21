/*
 * VFS Server IPC Contract
 *
 * VFS Server is a passive seL4 protection domain that provides a
 * POSIX-flavoured virtual filesystem to all other PDs.  Two backends:
 *   MEM  — fully in-memory, always available.
 *   BLK  — thin wrapper over virtio_blk (delegated via CH_VIRTIO_BLK).
 *
 * Channel: CH_VFS_SERVER (19) — controller PPCs into vfs_server.
 *
 * Shared memory:
 *   vfs_io_shmem  (128KB) — path strings, read/write data, and directory
 *                           listing entries all transit through this region.
 *   blk_dma_shmem         — shared with virtio_blk for DMA-backed block I/O.
 *
 * Invariants:
 *   - Paths must be written into vfs_io_shmem as NUL-terminated strings
 *     before any call that requires a path argument.
 *   - READ/WRITE data also flows through vfs_io_shmem; the caller is
 *     responsible for not overwriting path bytes before the server reads them.
 *   - File handles are opaque 32-bit integers assigned by the server.
 *   - Handles remain valid until explicitly closed or the server restarts.
 *   - OP_VFS_STAT, OP_VFS_UNLINK, OP_VFS_MKDIR, OP_VFS_READDIR use paths
 *     in shmem rather than handles; path_len is inferred by NUL scanning.
 *   - VFS_VERSION is returned in MR2 of the OP_VFS_HEALTH reply.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define VFS_CH_CONTROLLER   CH_VFS_SERVER   /* controller → vfs_server */

/* ─── Version ────────────────────────────────────────────────────────────── */
#define VFS_VERSION   1u

/* ─── Open flags (MR1 of OP_VFS_OPEN) ───────────────────────────────────── */
#define VFS_O_RDONLY   0x00u
#define VFS_O_WRONLY   0x01u
#define VFS_O_RDWR     0x02u
#define VFS_O_CREAT    0x04u
#define VFS_O_TRUNC    0x08u
#define VFS_O_APPEND   0x10u

/* ─── Opcodes (placed in MR0) ────────────────────────────────────────────── */
#define OP_VFS_OPEN     0x10u  /* open (or create) a file by path in shmem */
#define OP_VFS_CLOSE    0x11u  /* close an open file handle */
#define OP_VFS_READ     0x12u  /* read bytes from a file into shmem */
#define OP_VFS_WRITE    0x13u  /* write bytes from shmem into a file */
#define OP_VFS_STAT     0x14u  /* stat a path (path in shmem) */
#define OP_VFS_UNLINK   0x15u  /* delete a file (path in shmem) */
#define OP_VFS_MKDIR    0x16u  /* create a directory (path in shmem) */
#define OP_VFS_READDIR  0x17u  /* list directory entries into shmem */
#define OP_VFS_TRUNCATE 0x18u  /* truncate an open file to a given size */
#define OP_VFS_SYNC     0x19u  /* flush dirty blocks to the block backend */
#define OP_VFS_MOUNT    0x1Au  /* mount a backend at a prefix path in shmem */
#define OP_VFS_HEALTH   0x1Bu  /* return open_handle_count + VFS_VERSION */

/* ─── Request structs ────────────────────────────────────────────────────── */

/* OP_VFS_OPEN
 * MR0=op, MR1=flags   (path in vfs_io_shmem)
 */
struct __attribute__((packed)) vfs_server_req_open {
    uint32_t op;    /* OP_VFS_OPEN */
    uint32_t flags; /* VFS_O_* bitmask */
};

/* OP_VFS_CLOSE
 * MR0=op, MR1=handle
 */
struct __attribute__((packed)) vfs_server_req_close {
    uint32_t op;
    uint32_t handle;
};

/* OP_VFS_READ
 * MR0=op, MR1=handle, MR2=offset_lo, MR3=offset_hi, MR4=length
 */
struct __attribute__((packed)) vfs_server_req_read {
    uint32_t op;
    uint32_t handle;
    uint32_t offset_lo;
    uint32_t offset_hi;
    uint32_t length;
};

/* OP_VFS_WRITE
 * MR0=op, MR1=handle, MR2=offset_lo, MR3=offset_hi, MR4=length
 * (data placed in vfs_io_shmem before the call)
 */
struct __attribute__((packed)) vfs_server_req_write {
    uint32_t op;
    uint32_t handle;
    uint32_t offset_lo;
    uint32_t offset_hi;
    uint32_t length;
};

/* OP_VFS_STAT
 * MR0=op, MR1=handle  (handle 0 = use path in shmem)
 */
struct __attribute__((packed)) vfs_server_req_stat {
    uint32_t op;
    uint32_t handle;
};

/* OP_VFS_UNLINK
 * MR0=op  (path in shmem)
 */
struct __attribute__((packed)) vfs_server_req_unlink {
    uint32_t op;
};

/* OP_VFS_MKDIR
 * MR0=op  (path in shmem)
 */
struct __attribute__((packed)) vfs_server_req_mkdir {
    uint32_t op;
};

/* OP_VFS_READDIR
 * MR0=op  (path in shmem; entries returned in shmem)
 */
struct __attribute__((packed)) vfs_server_req_readdir {
    uint32_t op;
};

/* OP_VFS_TRUNCATE
 * MR0=op, MR1=handle, MR2=size_lo, MR3=size_hi
 */
struct __attribute__((packed)) vfs_server_req_truncate {
    uint32_t op;
    uint32_t handle;
    uint32_t size_lo;
    uint32_t size_hi;
};

/* OP_VFS_SYNC
 * MR0=op, MR1=handle  (handle 0 = sync all)
 */
struct __attribute__((packed)) vfs_server_req_sync {
    uint32_t op;
    uint32_t handle;
};

/* OP_VFS_MOUNT
 * MR0=op, MR1=backend_type  (prefix path in shmem)
 */
struct __attribute__((packed)) vfs_server_req_mount {
    uint32_t op;
    uint32_t backend_type; /* 0=MEM 1=BLK */
};

/* OP_VFS_HEALTH
 * MR0=op
 */
struct __attribute__((packed)) vfs_server_req_health {
    uint32_t op;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

/* OP_VFS_OPEN reply: MR0=ok, MR1=handle */
struct __attribute__((packed)) vfs_server_reply_open {
    uint32_t ok;
    uint32_t handle;
};

/* OP_VFS_CLOSE reply: MR0=ok */
struct __attribute__((packed)) vfs_server_reply_close {
    uint32_t ok;
};

/* OP_VFS_READ reply: MR0=ok, MR1=bytes_read  (data in vfs_io_shmem) */
struct __attribute__((packed)) vfs_server_reply_read {
    uint32_t ok;
    uint32_t bytes_read;
};

/* OP_VFS_WRITE reply: MR0=ok, MR1=bytes_written */
struct __attribute__((packed)) vfs_server_reply_write {
    uint32_t ok;
    uint32_t bytes_written;
};

/* OP_VFS_STAT reply: MR0=ok, MR1=size_lo, MR2=size_hi, MR3=mode */
struct __attribute__((packed)) vfs_server_reply_stat {
    uint32_t ok;
    uint32_t size_lo;
    uint32_t size_hi;
    uint32_t mode;   /* POSIX-style mode bits */
};

/* OP_VFS_UNLINK reply: MR0=ok */
struct __attribute__((packed)) vfs_server_reply_unlink {
    uint32_t ok;
};

/* OP_VFS_MKDIR reply: MR0=ok */
struct __attribute__((packed)) vfs_server_reply_mkdir {
    uint32_t ok;
};

/* OP_VFS_READDIR reply: MR0=ok, MR1=count  (entries in vfs_io_shmem) */
struct __attribute__((packed)) vfs_server_reply_readdir {
    uint32_t ok;
    uint32_t count;
};

/* OP_VFS_TRUNCATE reply: MR0=ok */
struct __attribute__((packed)) vfs_server_reply_truncate {
    uint32_t ok;
};

/* OP_VFS_SYNC reply: MR0=ok */
struct __attribute__((packed)) vfs_server_reply_sync {
    uint32_t ok;
};

/* OP_VFS_MOUNT reply: MR0=ok */
struct __attribute__((packed)) vfs_server_reply_mount {
    uint32_t ok;
};

/* OP_VFS_HEALTH reply: MR0=ok, MR1=open_handle_count, MR2=VFS_VERSION */
struct __attribute__((packed)) vfs_server_reply_health {
    uint32_t ok;
    uint32_t open_handle_count;
    uint32_t vfs_version;
};

/* ─── Shmem layout: directory entry (OP_VFS_READDIR) ────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t size_lo;
    uint32_t size_hi;
    uint32_t mode;
    uint8_t  name[56];   /* NUL-terminated filename */
} vfs_dirent_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum vfs_server_error {
    VFS_OK           = 0,
    VFS_ERR_INVAL    = 1,
    VFS_ERR_NOT_FOUND= 2,
    VFS_ERR_NO_HANDLES=3,
    VFS_ERR_NO_SPACE = 4,
    VFS_ERR_IO       = 5,
    VFS_ERR_PERM     = 6,
    VFS_ERR_NOT_DIR  = 7,
    VFS_ERR_EXISTS   = 8,
};
