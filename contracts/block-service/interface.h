/*
 * contracts/block-service/interface.h — Block Storage Generic Device Interface
 *
 * // STATUS: IMPLEMENTED
 *
 * This is the canonical contract for the block-service device service in agentOS.
 * The concrete implementation lives in kernel/agentos-root-task/src/virtio_blk.c,
 * with the VFS layer in vfs_server.c.
 *
 * The block-service provides sector-granularity read/write access to the virtio-blk
 * device, as well as cache flush, device geometry queries, and TRIM (discard)
 * for SSDs.  Every guest OS and VMM MUST use this service for block storage access
 * rather than configuring a virtio-blk queue directly.
 *
 * IPC transport:
 *   - Protected procedure call (PPC) via seL4 Microkit.
 *   - MR0 = opcode (BLK_SVC_OP_*)
 *   - MR1..MR6 = arguments (opcode-specific, see per-op comments below)
 *   - Reply: MR0 = status (BLK_SVC_ERR_*), MR1..MR6 = result fields
 *
 * Shared DMA memory (blk_dma_shmem, 32 KB):
 *   Data for READ_BLOCK and WRITE_BLOCK operations is transferred through a
 *   shared DMA region mapped into both the block-service PD and the calling PD.
 *   The physical address equals the virtual address (fixed_mr in the .system
 *   file) so the virtio device can DMA directly without IOMMU translation.
 *   Maximum transfer per call: BLK_SVC_DMA_SIZE bytes.
 *
 * Capability grant:
 *   vm_manager.c grants a PPC capability to the block-service endpoint and a
 *   read-write mapping of blk_dma_shmem at guest OS creation time.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

/* ── Interface version ──────────────────────────────────────────────────── */
#define BLK_SVC_INTERFACE_VERSION       1

/* ── Geometry constants ─────────────────────────────────────────────────── */
#define BLK_SVC_SECTOR_SIZE             512u        /* bytes per sector (virtio-blk standard) */
#define BLK_SVC_DMA_SIZE                0x8000u     /* 32 KB shared DMA window */
#define BLK_SVC_MAX_SECTORS_PER_OP      (BLK_SVC_DMA_SIZE / BLK_SVC_SECTOR_SIZE)

/* ── Opcodes (MR0) ──────────────────────────────────────────────────────── */

/*
 * BLK_SVC_OP_READ_BLOCK (0xF0)
 * Read one or more 512-byte sectors into the shared DMA region.
 *
 * The caller specifies the starting sector as a 64-bit value split across
 * two 32-bit message registers (required by seL4's MR width).
 * Data is placed at offset 0 in blk_dma_shmem.
 *
 * Request:
 *   MR1 = sector_lo    — low 32 bits of starting sector number
 *   MR2 = sector_hi    — high 32 bits
 *   MR3 = count        — number of sectors (max BLK_SVC_MAX_SECTORS_PER_OP)
 * Reply:
 *   MR0 = status
 *   MR1 = sectors_read — actual sectors transferred (may be < count on error)
 */
#define BLK_SVC_OP_READ_BLOCK           0xF0u

/*
 * BLK_SVC_OP_WRITE_BLOCK (0xF1)
 * Write one or more 512-byte sectors from the shared DMA region.
 *
 * The caller populates blk_dma_shmem at offset 0 before issuing this call.
 *
 * Request:
 *   MR1 = sector_lo    — low 32 bits of starting sector number
 *   MR2 = sector_hi    — high 32 bits
 *   MR3 = count        — number of sectors
 * Reply:
 *   MR0 = status
 *   MR1 = sectors_written
 */
#define BLK_SVC_OP_WRITE_BLOCK          0xF1u

/*
 * BLK_SVC_OP_FLUSH (0xF2)
 * Flush the device's volatile write cache to persistent storage.
 *
 * Only meaningful when VIRTIO_BLK_F_FLUSH was negotiated.  If the device
 * has no volatile cache this is a harmless no-op.
 *
 * Request: (none beyond MR0)
 * Reply:
 *   MR0 = status
 */
#define BLK_SVC_OP_FLUSH                0xF2u

/*
 * BLK_SVC_OP_GET_GEOMETRY (0xF3)
 * Query device capacity and block parameters.
 *
 * Request: (none beyond MR0)
 * Reply:
 *   MR0 = status
 *   MR1 = capacity_lo  — low 32 bits of total 512-byte sector count
 *   MR2 = capacity_hi  — high 32 bits
 *   MR3 = block_size   — preferred I/O block size in bytes
 *   MR4 = flags        — BLK_SVC_FLAG_* bitmask
 */
#define BLK_SVC_OP_GET_GEOMETRY         0xF3u

/* Geometry flags (MR4 of GET_GEOMETRY reply) */
#define BLK_SVC_FLAG_READ_ONLY          (1u << 0)  /* device is read-only */
#define BLK_SVC_FLAG_TRIM_SUPPORTED     (1u << 1)  /* TRIM/discard available */
#define BLK_SVC_FLAG_FLUSH_SUPPORTED    (1u << 2)  /* volatile write cache present */

/*
 * BLK_SVC_OP_TRIM (0xF4)
 * Discard (TRIM) a range of sectors, hinting to the device that the data
 * is no longer needed (e.g., after a file deletion).
 *
 * Only valid when BLK_SVC_FLAG_TRIM_SUPPORTED is set in the geometry flags.
 * The device is free to ignore TRIM on media that does not benefit from it.
 *
 * Request:
 *   MR1 = sector_lo    — low 32 bits of starting sector
 *   MR2 = sector_hi    — high 32 bits
 *   MR3 = count        — number of sectors to discard
 * Reply:
 *   MR0 = status
 */
#define BLK_SVC_OP_TRIM                 0xF4u

/*
 * BLK_SVC_OP_HEALTH (0xF5)
 * Query driver health counters for monitoring and diagnostics.
 *
 * Request: (none)
 * Reply:
 *   MR0 = status
 *   MR1 = initialized  — 1 if device is ready, 0 if not
 *   MR2 = error_count  — cumulative I/O errors since boot
 *   MR3 = queue_depth  — number of requests currently in flight (0 for sync driver)
 */
#define BLK_SVC_OP_HEALTH               0xF5u

/* ── Error / status codes (MR0 in replies) ──────────────────────────────── */
#define BLK_SVC_ERR_OK                  0u   /* success */
#define BLK_SVC_ERR_IO                  1u   /* device I/O error or timeout */
#define BLK_SVC_ERR_OOB                 2u   /* request exceeds device capacity */
#define BLK_SVC_ERR_NODEV               3u   /* device not initialised / absent */
#define BLK_SVC_ERR_INVAL               4u   /* invalid argument (zero count, etc.) */
#define BLK_SVC_ERR_PERM                5u   /* capability check failed */
#define BLK_SVC_ERR_RO                  6u   /* write attempted on read-only device */
#define BLK_SVC_ERR_UNSUP               7u   /* operation not supported by device */

/* ── Request / reply structs ─────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;
    uint32_t sector_lo;   /* starting sector (low 32 bits) */
    uint32_t sector_hi;   /* starting sector (high 32 bits) */
    uint32_t count;       /* sector count or discard count */
} blk_svc_req_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* BLK_SVC_ERR_* */
    uint32_t count;           /* sectors_read / sectors_written */
    uint32_t capacity_lo;     /* GET_GEOMETRY: total sectors low */
    uint32_t capacity_hi;     /* GET_GEOMETRY: total sectors high */
    uint32_t block_size;      /* GET_GEOMETRY: preferred block size */
    uint32_t flags;           /* GET_GEOMETRY: BLK_SVC_FLAG_* */
} blk_svc_reply_t;
