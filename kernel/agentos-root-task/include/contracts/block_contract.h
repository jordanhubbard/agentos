#pragma once
/* BLOCK_PD contract — version 1
 * PD: block_pd | Source: src/block_pd.c | Channel: CH_BLOCK_PD (63) from controller
 * Provides OS-neutral IPC block device access. LBA-addressed sector I/O via shared memory.
 */
#include <stdint.h>
#include <stdbool.h>

#define BLOCK_PD_CONTRACT_VERSION 1

/* ── Channel ─────────────────────────────────────────────────────────────── */
#define CH_BLOCK_PD  63u

/* ── Opcodes (from agentos_msg_tag_t) ───────────────────────────────────── */
#define BLOCK_OP_OPEN    0x1020u
#define BLOCK_OP_CLOSE   0x1021u
#define BLOCK_OP_READ    0x1022u
#define BLOCK_OP_WRITE   0x1023u
#define BLOCK_OP_FLUSH   0x1024u
#define BLOCK_OP_STATUS  0x1025u
#define BLOCK_OP_TRIM    0x1026u

#define BLOCK_MAX_HANDLES    4u
#define BLOCK_MAX_SECTORS    128u  /* max sectors per single READ/WRITE */
#define BLOCK_SECTOR_SIZE    512u

/* ── Request structs ─────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t op;            /* BLOCK_OP_OPEN */
    uint32_t dev_index;     /* physical device index (0-based) */
    uint32_t partition;     /* partition number (0 = whole disk) */
} block_req_open_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* BLOCK_OP_CLOSE */
    uint32_t handle;
} block_req_close_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* BLOCK_OP_READ */
    uint32_t handle;
    uint32_t lba_lo;        /* 64-bit LBA low word */
    uint32_t lba_hi;        /* 64-bit LBA high word */
    uint32_t sectors;       /* number of sectors, max BLOCK_MAX_SECTORS */
} block_req_read_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* BLOCK_OP_WRITE */
    uint32_t handle;
    uint32_t lba_lo;
    uint32_t lba_hi;
    uint32_t sectors;       /* data in shmem at block_shmem base */
} block_req_write_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* BLOCK_OP_FLUSH */
    uint32_t handle;
} block_req_flush_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* BLOCK_OP_STATUS */
    uint32_t handle;
} block_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* BLOCK_OP_TRIM */
    uint32_t handle;
    uint32_t lba_lo;
    uint32_t lba_hi;
    uint32_t sectors;
} block_req_trim_t;

/* ── Reply structs ───────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t result;        /* 0 = ok */
    uint32_t handle;        /* assigned handle */
} block_reply_open_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t sectors_read;
} block_reply_read_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t sectors_written;
} block_reply_write_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t sector_count_lo; /* total sectors (64-bit split) */
    uint32_t sector_count_hi;
    uint32_t sector_size;     /* bytes per sector */
    uint32_t read_only;       /* 1 if device is read-only */
} block_reply_status_t;

/* ── Error codes ─────────────────────────────────────────────────────────── */
typedef enum {
    BLOCK_OK               = 0,
    BLOCK_ERR_NO_HANDLE    = 1,  /* no free handle slots */
    BLOCK_ERR_BAD_HANDLE   = 2,  /* invalid handle */
    BLOCK_ERR_BAD_LBA      = 3,  /* LBA out of range */
    BLOCK_ERR_HW           = 4,  /* hardware / driver error */
    BLOCK_ERR_NOT_IMPL     = 5,  /* operation not yet implemented */
    BLOCK_ERR_READ_ONLY    = 6,  /* write attempted on read-only device */
    BLOCK_ERR_TOO_MANY     = 7,  /* sectors > BLOCK_MAX_SECTORS */
} block_error_t;

/* ── Invariants ──────────────────────────────────────────────────────────
 * - READ data is placed in block_shmem starting at offset 0; caller reads after reply.
 * - WRITE data must be placed in block_shmem by caller before IPC call.
 * - LBA + sectors must not exceed the partition boundary tracked at OPEN time.
 * - FLUSH must be called before power loss for durability guarantees.
 */
