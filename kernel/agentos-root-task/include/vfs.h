/*
 * agentOS VFS Server — Public Interface
 *
 * The VFS Server is a passive PD providing a POSIX-flavoured filesystem
 * interface to other protection domains.  Callers issue PPCs with VFS
 * opcodes; data larger than a few MRs is exchanged through the shared
 * vfs_io_shmem region (128 KB).
 *
 * Two backends are supported:
 *   MEM  — in-memory filesystem (always available, up to 128 inodes)
 *   BLK  — virtio-blk backend (optional; stubbed if device absent at init)
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── VFS version ──────────────────────────────────────────────────────────── */
#define VFS_VERSION         1u

/* ── Channel IDs (from VFS Server's perspective) ─────────────────────────── */
#define VFS_CH_CONTROLLER   0   /* controller PPCs in */
#define VFS_CH_INIT_AGENT   1   /* init_agent PPCs in */
#define VFS_CH_WORKER_0     2
#define VFS_CH_WORKER_1     3
#define VFS_CH_WORKER_2     4
#define VFS_CH_WORKER_3     5
#define VFS_CH_WORKER_4     6
#define VFS_CH_WORKER_5     7
#define VFS_CH_WORKER_6     8
#define VFS_CH_WORKER_7     9
#define VFS_CH_SPAWN_SERVER 10  /* SpawnServer reads ELFs */
#define VFS_CH_APP_MANAGER  11  /* AppManager mounts */
#define VFS_CH_VIRTIO_BLK   12  /* VFS PPCs OUT into virtio-blk */

/* Console slot / pd_id for this PD */
#define VFS_CONSOLE_SLOT    14u
#define VFS_PD_ID           14u

/* ── VFS opcodes (label / MR0 in PPC calls) ──────────────────────────────── */
#define OP_VFS_OPEN         0xE0u
#define OP_VFS_CLOSE        0xE1u
#define OP_VFS_READ         0xE2u
#define OP_VFS_WRITE        0xE3u
#define OP_VFS_STAT         0xE4u
#define OP_VFS_UNLINK       0xE5u
#define OP_VFS_MKDIR        0xE6u
#define OP_VFS_READDIR      0xE7u
#define OP_VFS_TRUNCATE     0xE8u
#define OP_VFS_SYNC         0xE9u
#define OP_VFS_MOUNT        0xEAu
#define OP_VFS_HEALTH       0xEBu

/* ── Open flags (MR1 for OP_VFS_OPEN) ───────────────────────────────────── */
#define VFS_O_RD            (1u << 0)
#define VFS_O_WR            (1u << 1)
#define VFS_O_CREAT         (1u << 2)
#define VFS_O_APPEND        (1u << 3)
#define VFS_O_DIR           (1u << 4)

/* ── Return codes (MR0 in VFS replies) ───────────────────────────────────── */
#define VFS_OK              0u
#define VFS_ERR_NOT_FOUND   1u
#define VFS_ERR_EXISTS      2u
#define VFS_ERR_NO_HANDLES  3u
#define VFS_ERR_IO          4u
#define VFS_ERR_PERM        5u
#define VFS_ERR_INVAL       6u
#define VFS_ERR_NO_SPACE    7u
#define VFS_ERR_NOT_DIR     8u

/* ── Backend types ───────────────────────────────────────────────────────── */
#define VFS_BACKEND_MEM     0u
#define VFS_BACKEND_BLK     1u

/* ── virtio-blk sub-interface opcodes (MR0 for PPCs on VFS_CH_VIRTIO_BLK) ── */
#define OP_BLK_READ         0xF0u
#define OP_BLK_WRITE        0xF1u
#define OP_BLK_FLUSH        0xF2u
#define OP_BLK_INFO         0xF3u

/* virtio-blk result codes (label on reply) */
#define BLK_OK              0u
#define BLK_ERR_IO          1u
#define BLK_ERR_OOB         2u
#define BLK_ERR_NODEV       3u

/* ── Shared memory layout constants ──────────────────────────────────────── */
#define VFS_SHMEM_SIZE          0x20000u  /* 128 KB */

/* Offsets into vfs_io_shmem */
#define VFS_SHMEM_REQ_OFF       0u        /* vfs_req_t (48 bytes) */
#define VFS_SHMEM_PATH_OFF      48u       /* path buffer (256 bytes) */
#define VFS_SHMEM_READDIR_OFF   304u      /* readdir buffer (512 bytes) */
#define VFS_SHMEM_DATA_OFF      816u      /* data buffer (~128 KB) */

#define VFS_SHMEM_PATH_MAX      256u
#define VFS_SHMEM_READDIR_MAX   512u
#define VFS_SHMEM_DATA_MAX      (VFS_SHMEM_SIZE - VFS_SHMEM_DATA_OFF)  /* 130255 bytes */

/* ── Request descriptor (lives at vfs_io_shmem + VFS_SHMEM_REQ_OFF) ─────── */
typedef struct __attribute__((packed)) {
    uint32_t op;          /* VFS opcode */
    uint32_t flags;       /* open flags or mode flags */
    uint32_t handle;      /* file handle */
    uint32_t path_len;    /* length of path string at shmem[48] */
    uint64_t offset;      /* file offset */
    uint32_t data_len;    /* length of data at shmem[816] */
    uint32_t result;      /* result code (written by server) */
    uint64_t file_size;   /* file size (written by server on stat) */
    uint32_t mode;        /* file mode / type flags */
    uint8_t  _pad[4];
} vfs_req_t;              /* exactly 48 bytes */

/* ── Handle table entry ──────────────────────────────────────────────────── */
#define VFS_MAX_HANDLES     64u

typedef struct {
    bool     active;
    uint8_t  backend;   /* VFS_BACKEND_* */
    uint32_t inode;     /* backend inode number */
    uint32_t flags;     /* open flags */
    uint64_t offset;    /* current seek position (for sequential callers) */
} vfs_handle_t;

/* ── MemBackend inode ─────────────────────────────────────────────────────── */
#define VFS_MEM_MAX_INODES  128u
#define VFS_MEM_FILE_SIZE   32768u  /* 32 KB per file */
#define VFS_MEM_NAME_MAX    64u

typedef struct {
    bool     active;
    bool     is_dir;
    char     name[VFS_MEM_NAME_MAX];
    uint32_t parent_ino;
    uint32_t size;
    uint8_t  data[VFS_MEM_FILE_SIZE];
} mem_inode_t;

/* ── Mount table entry ───────────────────────────────────────────────────── */
#define VFS_MAX_MOUNTS      4u

typedef struct {
    bool     active;
    char     prefix[VFS_MEM_NAME_MAX];
    uint8_t  backend_type;  /* VFS_BACKEND_* */
} vfs_mount_t;
