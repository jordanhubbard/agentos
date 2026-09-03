/*
 * agentOS blk_virt queue ABI
 *
 * Binary-compatible with sDDF blk_req_t / blk_resp_t / blk_storage_info_t
 * (transfer 4096, queue capacity 16). This header must not include seL4 or
 * sddf headers so host tests can compile it.
 *
 * Guest IPA 0x0A020000 is the emulated virtio-mmio blk device (faults to the
 * VMM). It must not overlap QEMU's virtio-mmio window at 0x0A000000 or the
 * emulated net device at 0x0A010000. QEMU virtio-blk (slot 1 / vda) is a
 * kill-dated crutch and remains the guest boot disk this pass.
 */

#ifndef AOS_PLATFORM_BLK_LAYOUT_H
#define AOS_PLATFORM_BLK_LAYOUT_H

#include <stdint.h>
#include <stdbool.h>

#define AOS_BLK_TRANSFER_SIZE        4096u
#define AOS_BLK_SECTOR_SIZE          512u
#define AOS_BLK_QUEUE_CAPACITY       16u
#define AOS_BLK_QUEUE_BYTES          0x1000u
#define AOS_BLK_MAX_CLIENTS          4u
#define AOS_BLK_DISK_BLOCKS          64u    /* 256 KB RAM disk */
#define AOS_BLK_DISK_BYTES           (AOS_BLK_DISK_BLOCKS * AOS_BLK_TRANSFER_SIZE)
#define AOS_BLK_DATA_BYTES           (AOS_BLK_QUEUE_CAPACITY * AOS_BLK_TRANSFER_SIZE)
#define AOS_BLK_CLIENT_STRIDE        0x20000u   /* 128 KB per client */
#define AOS_BLK_SHMEM_SIZE           0x100000u  /* 1 MB */
#define AOS_BLK_SHMEM_VA             0x20200000UL /* after net_virt; future MR */

#define AOS_BLK_DISK_OFF             0x0000u
#define AOS_BLK_CLIENT_BASE          0x40000u   /* after 256 KB disk */

#define AOS_BLK_STORAGE_INFO_OFF     0x0000u    /* within client stride */
#define AOS_BLK_REQ_QUEUE_OFF        0x1000u
#define AOS_BLK_RESP_QUEUE_OFF       0x2000u
#define AOS_BLK_DATA_OFF             0x3000u

#define AOS_BLK_STORAGE_INFO_BYTES   0x1000u
#define AOS_BLK_MAX_SERIAL           63u

/*
 * Emulated virtio-mmio blk — must NOT overlap QEMU 0x0A000000 or net 0x0A010000.
 *
 * GIC: SPI 20 → INTID 52. SPI 16–19 are QEMU virtio-mmio slots 0–3
 * (net crutch, blk0/vda crutch, cc_pd reserved / emulated net IRQ, seed blk).
 */
#define AOS_VIRTIO_BLK_GUEST_IPA     0x0A020000UL
#define AOS_VIRTIO_BLK_MMIO_SIZE     0x1000UL
#define AOS_VIRTIO_BLK_VIRQ          52u        /* GIC SPI 20 */
#define AOS_VIRTIO_BLK_DTB_SPI       20u

typedef enum aos_blk_req_code {
    AOS_BLK_REQ_READ = 0,
    AOS_BLK_REQ_WRITE = 1,
    AOS_BLK_REQ_FLUSH = 2,
    AOS_BLK_REQ_BARRIER = 3
} aos_blk_req_code_t;

typedef enum aos_blk_resp_status {
    AOS_BLK_RESP_OK = 0,
    AOS_BLK_RESP_ERR_UNSPEC = 1,
    AOS_BLK_RESP_ERR_INVALID_PARAM = 2,
    AOS_BLK_RESP_ERR_IO = 3,
    AOS_BLK_RESP_ERR_NO_DEVICE = 4
} aos_blk_resp_status_t;

/*
 * Match sDDF blk_req_t: enum + two uint64 + uint16 + uint32, natural align
 * (sizeof 32). Do not pack.
 */
typedef struct aos_blk_req {
    aos_blk_req_code_t code;
    uint64_t io_or_offset;
    uint64_t block_number;
    uint16_t count;
    uint32_t id;
} aos_blk_req_t;

typedef struct aos_blk_resp {
    aos_blk_resp_status_t status;
    uint16_t success_count;
    uint32_t id;
} aos_blk_resp_t;

typedef struct aos_blk_req_queue {
    uint32_t head;
    uint32_t tail;
    bool plugged;
    aos_blk_req_t buffers[];
} aos_blk_req_queue_t;

typedef struct aos_blk_resp_queue {
    uint32_t head;
    uint32_t tail;
    aos_blk_resp_t buffers[];
} aos_blk_resp_queue_t;

/* Match sDDF blk_storage_info_t (sizeof 88, capacity at offset 80). */
typedef struct aos_blk_storage_info {
    char serial_number[AOS_BLK_MAX_SERIAL + 1u];
    bool read_only;
    bool ready;
    uint16_t sector_size;
    uint16_t block_size;
    uint16_t queue_depth;
    uint16_t cylinders, heads, blocks;
    uint64_t capacity;
} aos_blk_storage_info_t;

typedef struct aos_blk_virt_client {
    aos_blk_storage_info_t *info;
    aos_blk_req_queue_t    *req;
    aos_blk_resp_queue_t   *resp;
    uint8_t                *data;
    uint32_t                capacity;
} aos_blk_virt_client_t;

typedef struct aos_blk_virt {
    aos_blk_virt_client_t clients[AOS_BLK_MAX_CLIENTS];
    uint32_t num_clients;
    uint8_t *disk;
    uint32_t disk_blocks;
} aos_blk_virt_t;

#endif /* AOS_PLATFORM_BLK_LAYOUT_H */
