/*
 * virtio-blk Block Device Driver IPC Contract
 *
 * virtio_blk is a passive seL4 protection domain that implements the
 * virtio-MMIO block device driver for agentOS.  It is the canonical block
 * device service for the platform and is shared by vfs_server (via
 * blk_dma_shmem) and any other PD that needs raw block I/O.
 *
 * Channel: VIRTIO_BLK_CH_CONTROLLER (0) — controller PPCs into virtio_blk.
 *
 * Shared memory:
 *   Each media has a private 32KB DMA window inside blk_dma_shmem. On
 *   OP_BLK_READ the driver writes block data to the selected media window.
 *   On OP_BLK_WRITE the caller must populate that media's window first.
 *
 * Invariants:
 *   - media_id selects one canonical block-service medium.
 *   - block_lo / block_hi are the low and high 32-bit halves of the 64-bit
 *     logical block address (LBA).
 *   - count is the number of 512-byte sectors to transfer; must not exceed
 *     blk_dma_shmem capacity (32768 / 512 = 64 sectors max per call).
 *   - OP_BLK_FLUSH issues a cache-flush to stable storage and is a no-op
 *     if the backend does not support it.
 *   - OP_BLK_INFO returns geometry; capacity_lo / capacity_hi form a 64-bit
 *     sector count and block_size is in bytes.
 *   - error_count in OP_BLK_HEALTH is a saturating counter of I/O errors
 *     since the last power-on or explicit reset.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define VIRTIO_BLK_CH_CONTROLLER   0u   /* controller → virtio_blk (from virtio_blk's perspective) */

#define VIRTIO_BLK_CONTRACT_VERSION 2u
#define BLK_MEDIA_UBUNTU_INSTALL    0u
#define BLK_MEDIA_FREEBSD_INSTALL   1u
#define BLK_MEDIA_COUNT             2u

/* ─── Opcodes (placed in MR0) ────────────────────────────────────────────── */
#define OP_BLK_READ    0xF0u  /* read sectors from device into blk_dma_shmem */
#define OP_BLK_WRITE   0xF1u  /* write sectors from blk_dma_shmem to device */
#define OP_BLK_FLUSH   0xF2u  /* flush write cache to stable storage */
#define OP_BLK_INFO    0xF3u  /* return device geometry */
#define OP_BLK_HEALTH  0xF5u  /* return initialized flag and error_count */

/* ─── Request structs ────────────────────────────────────────────────────── */

/* OP_BLK_READ
 * MR0=op, MR1=block_lo, MR2=block_hi, MR3=count, MR4=media_id
 */
struct __attribute__((packed)) virtio_blk_req_read {
    uint32_t op;        /* OP_BLK_READ */
    uint32_t block_lo;  /* low 32 bits of LBA */
    uint32_t block_hi;  /* high 32 bits of LBA */
    uint32_t count;     /* sector count (512-byte sectors) */
    uint32_t media_id;  /* BLK_MEDIA_* */
};

/* OP_BLK_WRITE
 * MR0=op, MR1=block_lo, MR2=block_hi, MR3=count, MR4=media_id
 * (data placed in blk_dma_shmem before the call)
 */
struct __attribute__((packed)) virtio_blk_req_write {
    uint32_t op;
    uint32_t block_lo;
    uint32_t block_hi;
    uint32_t count;
    uint32_t media_id;
};

/* OP_BLK_FLUSH
 * MR0=op
 */
struct __attribute__((packed)) virtio_blk_req_flush {
    uint32_t op;
    uint32_t media_id;
};

/* OP_BLK_INFO
 * MR0=op
 */
struct __attribute__((packed)) virtio_blk_req_info {
    uint32_t op;
    uint32_t media_id;
};

/* OP_BLK_HEALTH
 * MR0=op
 */
struct __attribute__((packed)) virtio_blk_req_health {
    uint32_t op;
    uint32_t media_id;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

/* OP_BLK_READ reply: MR0=status  (data in blk_dma_shmem) */
struct __attribute__((packed)) virtio_blk_reply_read {
    uint32_t status;   /* enum virtio_blk_error */
};

/* OP_BLK_WRITE reply: MR0=status */
struct __attribute__((packed)) virtio_blk_reply_write {
    uint32_t status;
};

/* OP_BLK_FLUSH reply: MR0=status */
struct __attribute__((packed)) virtio_blk_reply_flush {
    uint32_t status;
};

/* OP_BLK_INFO reply: MR0=status, MR1=capacity_lo, MR2=capacity_hi, MR3=block_size */
struct __attribute__((packed)) virtio_blk_reply_info {
    uint32_t status;
    uint32_t capacity_lo;  /* low 32 bits of total sector count */
    uint32_t capacity_hi;  /* high 32 bits of total sector count */
    uint32_t block_size;   /* bytes per sector (typically 512) */
};

/* OP_BLK_HEALTH reply: MR0=ok, MR1=initialized, MR2=error_count */
struct __attribute__((packed)) virtio_blk_reply_health {
    uint32_t ok;
    uint32_t initialized;  /* 1 if device negotiation completed successfully */
    uint32_t error_count;  /* saturating count of I/O errors since power-on */
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum virtio_blk_error {
    BLK_OK        = 0,
    BLK_ERR_IO    = 1,  /* I/O error returned by virtio device */
    BLK_ERR_OOB   = 2,  /* LBA + count exceeds device capacity */
    BLK_ERR_NODEV = 3,  /* device not present or not initialised */
};
