/*
 * Block Device PD IPC Contract
 *
 * The block_pd owns block storage hardware exclusively via seL4 device frame
 * capabilities.  It provides partition-level block I/O to guest OSes.
 *
 * Channel: CH_BLOCK_PD (see agentos.h)
 * Opcodes: MSG_BLOCK_* (see agentos.h)
 *
 * Invariants:
 *   - MSG_BLOCK_OPEN returns a handle per partition; all subsequent calls use it.
 *   - Read/write data is transferred via the block_shmem shared memory region.
 *   - LBA addressing is 64-bit; MR2 carries lba_lo and a separate MR carries
 *     lba_hi for large drives (> 4 TB).
 *   - MSG_BLOCK_FLUSH guarantees persistence of all preceding writes.
 *   - MSG_BLOCK_TRIM is advisory; the PD may ignore it on non-SSD media.
 *   - The READONLY flag in MSG_BLOCK_STATUS reflects the hardware write-protect
 *     signal; writes to a read-only partition return BLOCK_ERR_READONLY.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define BLOCK_PD_CH_CONTROLLER  CH_BLOCK_PD

/* ─── Configuration ──────────────────────────────────────────────────────── */
#define BLOCK_MAX_CLIENTS    8u
#define BLOCK_MAX_SECTORS    128u  /* max sectors per read/write request */

/* ─── Request structs ────────────────────────────────────────────────────── */

struct block_req_open {
    uint32_t dev_id;            /* physical device index */
    uint32_t partition;         /* partition index (0 = whole device) */
    uint32_t flags;             /* BLOCK_OPEN_FLAG_* */
};

#define BLOCK_OPEN_FLAG_RDONLY  (1u << 0)

struct block_req_close {
    uint32_t handle;
};

struct block_req_read {
    uint32_t handle;
    uint32_t lba_lo;            /* sector address (low 32 bits) */
    uint32_t lba_hi;            /* sector address (high 32 bits) */
    uint32_t sectors;           /* sector count (max BLOCK_MAX_SECTORS) */
};

struct block_req_write {
    uint32_t handle;
    uint32_t lba_lo;
    uint32_t lba_hi;
    uint32_t sectors;
};

struct block_req_flush {
    uint32_t handle;
};

struct block_req_status {
    uint32_t handle;
};

struct block_req_trim {
    uint32_t handle;
    uint32_t lba_lo;
    uint32_t lba_hi;
    uint32_t sectors;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct block_reply_open {
    uint32_t ok;
    uint32_t handle;
};

struct block_reply_close {
    uint32_t ok;
};

struct block_reply_read {
    uint32_t ok;
    uint32_t actual;            /* sectors actually read */
};

struct block_reply_write {
    uint32_t ok;
    uint32_t actual;            /* sectors actually written */
};

struct block_reply_flush {
    uint32_t ok;
};

struct block_reply_status {
    uint32_t ok;
    uint32_t sector_count_lo;
    uint32_t sector_count_hi;
    uint32_t sector_size;       /* bytes per sector (typically 512 or 4096) */
    uint32_t flags;             /* BLOCK_STATUS_FLAG_* */
};

#define BLOCK_STATUS_FLAG_READONLY  (1u << 0)
#define BLOCK_STATUS_FLAG_REMOVABLE (1u << 1)
#define BLOCK_STATUS_FLAG_SSD       (1u << 2)

struct block_reply_trim {
    uint32_t ok;
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum block_error {
    BLOCK_OK                = 0,
    BLOCK_ERR_BAD_HANDLE    = 1,
    BLOCK_ERR_BAD_DEV       = 2,
    BLOCK_ERR_BAD_PART      = 3,
    BLOCK_ERR_READONLY      = 4,
    BLOCK_ERR_OOB           = 5,  /* LBA out of bounds */
    BLOCK_ERR_IO            = 6,  /* hardware I/O error */
    BLOCK_ERR_NO_SLOTS      = 7,
};
