/*
 * virtio_blk.h — virtio-blk block device driver for agentOS
 *
 * Defines MMIO offsets, virtqueue structures, opcodes, and return codes
 * for the virtio-blk protection domain.
 *
 * Targets QEMU virtio-MMIO transport v2 (magic 0x74726976, version 2).
 * All structures are packed to match the virtio spec wire layout exactly.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * virtio-MMIO register offsets (relative to MMIO base)
 * ──────────────────────────────────────────────────────────────────────────── */

#define VIRTIO_MMIO_MAGIC_VALUE         0x000u  /* RO: must read 0x74726976 "virt" */
#define VIRTIO_MMIO_VERSION             0x004u  /* RO: must read 2 */
#define VIRTIO_MMIO_DEVICE_ID           0x008u  /* RO: 2 = block device */
#define VIRTIO_MMIO_VENDOR_ID           0x00cu  /* RO: vendor identifier */
#define VIRTIO_MMIO_DEVICE_FEATURES     0x010u  /* RO: feature bits (low 32) */
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014u  /* WO: select feature word (0 or 1) */
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020u  /* WO: negotiated feature bits */
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024u  /* WO: select feature word (0 or 1) */
#define VIRTIO_MMIO_QUEUE_SEL           0x034u  /* WO: select queue index */
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x038u  /* RO: max queue size for selected queue */
#define VIRTIO_MMIO_QUEUE_NUM           0x03cu  /* WO: chosen queue size */
#define VIRTIO_MMIO_QUEUE_READY         0x044u  /* RW: write 1 to activate, read to confirm */
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050u  /* WO: write queue index to kick device */
#define VIRTIO_MMIO_INTERRUPT_STATUS    0x060u  /* RO: bit0=used_ring_update, bit1=config_change */
#define VIRTIO_MMIO_INTERRUPT_ACK       0x064u  /* WO: write to clear interrupt bits */
#define VIRTIO_MMIO_STATUS              0x070u  /* RW: device status register */
#define VIRTIO_MMIO_QUEUE_DESC_LOW      0x080u  /* WO: low 32 bits of descriptor table paddr */
#define VIRTIO_MMIO_QUEUE_DESC_HIGH     0x084u  /* WO: high 32 bits of descriptor table paddr */
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW     0x090u  /* WO: low 32 bits of available ring paddr */
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH    0x094u  /* WO: high 32 bits of available ring paddr */
#define VIRTIO_MMIO_QUEUE_USED_LOW      0x0a0u  /* WO: low 32 bits of used ring paddr */
#define VIRTIO_MMIO_QUEUE_USED_HIGH     0x0a4u  /* WO: high 32 bits of used ring paddr */
#define VIRTIO_MMIO_CONFIG_GENERATION   0x0acu  /* RO: configuration space generation counter */
#define VIRTIO_MMIO_CONFIG              0x100u  /* RO/RW: device-specific config space */

/* virtio-MMIO magic value */
#define VIRTIO_MMIO_MAGIC               0x74726976u  /* little-endian "virt" */

/* ─────────────────────────────────────────────────────────────────────────────
 * virtio Device Status Register bits (VIRTIO_MMIO_STATUS)
 * ──────────────────────────────────────────────────────────────────────────── */

#define VIRTIO_STATUS_ACKNOWLEDGE       (1u << 0)  /* Guest has noticed device */
#define VIRTIO_STATUS_DRIVER            (1u << 1)  /* Guest knows how to drive device */
#define VIRTIO_STATUS_DRIVER_OK         (1u << 2)  /* Driver is ready */
#define VIRTIO_STATUS_FEATURES_OK       (1u << 3)  /* Feature negotiation complete */
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET (1u << 6) /* Device has experienced an error */
#define VIRTIO_STATUS_FAILED            (1u << 7)  /* Guest has given up on the device */

/* ─────────────────────────────────────────────────────────────────────────────
 * virtio-blk Feature bits (device features, word 0)
 * ──────────────────────────────────────────────────────────────────────────── */

#define VIRTIO_BLK_F_SIZE_MAX           (1u << 1)   /* max segment size in size_max */
#define VIRTIO_BLK_F_SEG_MAX            (1u << 2)   /* max segments in seg_max */
#define VIRTIO_BLK_F_GEOMETRY           (1u << 4)   /* geometry fields are valid */
#define VIRTIO_BLK_F_RO                 (1u << 5)   /* device is read-only */
#define VIRTIO_BLK_F_BLK_SIZE           (1u << 6)   /* blk_size field is valid */
#define VIRTIO_BLK_F_FLUSH              (1u << 9)   /* device supports cache flush */
#define VIRTIO_BLK_F_TOPOLOGY           (1u << 10)  /* topology information valid */
#define VIRTIO_BLK_F_CONFIG_WCE         (1u << 11)  /* writeback mode configurable */

/* Features we will request from the device */
#define VIRTIO_BLK_FEATURES_WANTED \
    (VIRTIO_BLK_F_SIZE_MAX | VIRTIO_BLK_F_SEG_MAX | VIRTIO_BLK_F_BLK_SIZE)

/* ─────────────────────────────────────────────────────────────────────────────
 * virtio-blk device configuration space (at VIRTIO_MMIO_CONFIG offset 0x100)
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint64_t capacity;      /* total number of 512-byte sectors */
    uint32_t size_max;      /* max size of any single segment (if F_SIZE_MAX) */
    uint32_t seg_max;       /* max number of segments in a request (if F_SEG_MAX) */
    struct __attribute__((packed)) {
        uint16_t cylinders;
        uint8_t  heads;
        uint8_t  sectors;
    } geometry;             /* CHS geometry (if F_GEOMETRY) */
    uint32_t blk_size;      /* preferred I/O block size in bytes (if F_BLK_SIZE) */
    struct __attribute__((packed)) {
        uint8_t  physical_block_exp;
        uint8_t  alignment_offset;
        uint16_t min_io_size;
        uint32_t opt_io_size;
    } topology;
    uint8_t  writeback;
    uint8_t  _unused0[3];
    uint32_t max_discard_sectors;
    uint32_t max_discard_seg;
    uint32_t discard_sector_alignment;
    uint32_t max_write_zeroes_sectors;
    uint32_t max_write_zeroes_seg;
    uint8_t  write_zeroes_may_unmap;
    uint8_t  _unused1[3];
} virtio_blk_config_t;

/* Default block size when F_BLK_SIZE is not negotiated */
#define VIRTIO_BLK_DEFAULT_SECTOR_SIZE  512u

/* ─────────────────────────────────────────────────────────────────────────────
 * Virtqueue split-queue structures (virtio 1.1 spec §2.6)
 * ──────────────────────────────────────────────────────────────────────────── */

/* Virtqueue descriptor flags */
#define VIRTQ_DESC_F_NEXT               (1u << 0)  /* descriptor continues via .next */
#define VIRTQ_DESC_F_WRITE              (1u << 1)  /* device writes to this buffer */
#define VIRTQ_DESC_F_INDIRECT           (1u << 2)  /* buffer contains a list of descriptors */

/* One descriptor in the descriptor table (16 bytes, spec §2.6.5) */
typedef struct __attribute__((packed)) {
    uint64_t addr;   /* guest-physical address of the buffer */
    uint32_t len;    /* byte length of the buffer */
    uint16_t flags;  /* VIRTQ_DESC_F_* bitmask */
    uint16_t next;   /* index of next descriptor (if NEXT flag set) */
} virtq_desc_t;

/* Available ring header (driver→device, spec §2.6.6) */
typedef struct __attribute__((packed)) {
    uint16_t flags;     /* VIRTQ_AVAIL_F_NO_INTERRUPT (1) to suppress used-ring IRQs */
    uint16_t idx;       /* next slot driver will write */
    uint16_t ring[];    /* descriptor indices; actual length = queue_size */
} virtq_avail_t;

/* One element in the used ring returned by the device (8 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t id;    /* head descriptor index of the completed chain */
    uint32_t len;   /* total bytes written into device-writable descriptors */
} virtq_used_elem_t;

/* Used ring header (device→driver, spec §2.6.8) */
typedef struct __attribute__((packed)) {
    uint16_t flags;            /* VIRTQ_USED_F_NO_NOTIFY (1) if device wants no kicks */
    uint16_t idx;              /* next slot device will write */
    virtq_used_elem_t ring[];  /* actual length = queue_size */
} virtq_used_t;

/* Interrupt status bits */
#define VIRTIO_INTR_USED_RING_UPDATE    (1u << 0)
#define VIRTIO_INTR_CONFIG_CHANGE       (1u << 1)

/* ─────────────────────────────────────────────────────────────────────────────
 * virtio-blk request types and layout (spec §5.2.6)
 * ──────────────────────────────────────────────────────────────────────────── */

#define VIRTIO_BLK_T_IN                 0u   /* read sectors from device */
#define VIRTIO_BLK_T_OUT                1u   /* write sectors to device */
#define VIRTIO_BLK_T_FLUSH              4u   /* flush volatile write cache */

/* Request header (16 bytes, placed in the first descriptor) */
typedef struct __attribute__((packed)) {
    uint32_t type;      /* VIRTIO_BLK_T_* */
    uint32_t reserved;  /* must be zero */
    uint64_t sector;    /* starting sector number (512-byte units) */
} virtio_blk_req_hdr_t;

/* Request status byte values (last byte, returned in status descriptor) */
#define VIRTIO_BLK_S_OK                 0u   /* success */
#define VIRTIO_BLK_S_IOERR              1u   /* device or driver error */
#define VIRTIO_BLK_S_UNSUPP             2u   /* request type unsupported */

/* ─────────────────────────────────────────────────────────────────────────────
 * agentOS virtio-blk IPC opcodes (label field of microkit_msginfo)
 * ──────────────────────────────────────────────────────────────────────────── */

/*
 * OP_BLK_READ  — read count sectors starting at (block_hi<<32 | block_lo)
 *   MR1 = block_lo (low 32 bits of sector number)
 *   MR2 = block_hi (high 32 bits of sector number)
 *   MR3 = count    (number of 512-byte sectors)
 *   Data is placed in blk_dma_shmem starting at offset 0.
 *   Reply: MR0 = BLK_OK or error code
 */
#define OP_BLK_READ                     0xF0u

/*
 * OP_BLK_WRITE — write count sectors starting at (block_hi<<32 | block_lo)
 *   MR1 = block_lo
 *   MR2 = block_hi
 *   MR3 = count
 *   Data is sourced from blk_dma_shmem starting at offset 0.
 *   Reply: MR0 = BLK_OK or error code
 */
#define OP_BLK_WRITE                    0xF1u

/*
 * OP_BLK_FLUSH — flush volatile write cache
 *   Reply: MR0 = BLK_OK, or BLK_ERR_NODEV if device not initialised
 */
#define OP_BLK_FLUSH                    0xF2u

/*
 * OP_BLK_INFO — query device geometry
 *   Reply: MR0=BLK_OK, MR1=capacity_lo, MR2=capacity_hi, MR3=block_size
 */
#define OP_BLK_INFO                     0xF3u

/*
 * OP_BLK_HEALTH — query driver health counters
 *   Reply: MR0=BLK_OK, MR1=initialized(0/1), MR2=error_count
 */
#define OP_BLK_HEALTH                   0xF4u

/* ─────────────────────────────────────────────────────────────────────────────
 * virtio-blk return codes (placed in MR0 of PPC reply)
 * ──────────────────────────────────────────────────────────────────────────── */

#define BLK_OK                          0u   /* operation succeeded */
#define BLK_ERR_IO                      1u   /* device I/O error or timeout */
#define BLK_ERR_OOB                     2u   /* request exceeds device capacity */
#define BLK_ERR_NODEV                   3u   /* device not initialised / absent */

/* ─────────────────────────────────────────────────────────────────────────────
 * Channel assignments (from virtio_blk's own perspective)
 * ──────────────────────────────────────────────────────────────────────────── */

#define VIRTIO_BLK_CH_CONTROLLER        0u   /* pp=true inbound from controller (admin/status) */
#define VIRTIO_BLK_CH_VFS               1u   /* pp=true inbound from vfs_server (block I/O) */

/* Queue size for the MVP synchronous polling mode (single in-flight request) */
#define VIRTIO_BLK_QUEUE_SIZE           1u

/* Poll timeout: maximum spin iterations before declaring an I/O timeout */
#define VIRTIO_BLK_POLL_ITERS           100000u
