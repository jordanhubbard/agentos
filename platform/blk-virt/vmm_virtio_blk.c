/*
 * Guest-facing virtio-blk: libvmm device at AOS_VIRTIO_BLK_GUEST_IPA,
 * backend = sDDF queues + local aos_blk_virt_pump (RAM disk until blk_drv).
 *
 * QEMU virtio-mmio blk at 0x0A000200 remains a kill-dated passthrough
 * crutch on Ubuntu (guest vda). Buildroot DTB uses only this device.
 */

#include <libvmm/libvmm.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/block.h>
#include <sddf/blk/queue.h>
#include <sddf/blk/storage_info.h>
#include <platform/blk_layout.h>
#include <platform/blk_virt_pump.h>
#include <platform/blk_host_layout.h>
#include <platform/vmm_virtio_blk.h>

_Static_assert(AOS_BLK_TRANSFER_SIZE == BLK_TRANSFER_SIZE,
               "platform blk transfer size must match sDDF BLK_TRANSFER_SIZE");
_Static_assert(sizeof(aos_blk_req_t) == sizeof(blk_req_t),
               "aos_blk_req_t must match sDDF blk_req_t");
_Static_assert(sizeof(aos_blk_resp_t) == sizeof(blk_resp_t),
               "aos_blk_resp_t must match sDDF blk_resp_t");
_Static_assert(sizeof(aos_blk_storage_info_t) == sizeof(blk_storage_info_t),
               "aos_blk_storage_info_t must match sDDF blk_storage_info_t");

/*
 * Private RAM disk + queues. Not a system_desc MR this pass — adding one
 * would consume linux_vmm's last PD_MAX_MEMORY_REGIONS slot. Future:
 * map 2 MB at AOS_BLK_SHMEM_VA like net_virt.
 */
static uint8_t g_blk_region[AOS_BLK_SHMEM_SIZE] __attribute__((aligned(4096)));

static struct virtio_blk_device g_aos_blk;
static aos_blk_virt_t           g_aos_virt;
static aos_blk_virt_client_t    g_aos_client;
static blk_queue_handle_t       g_queue;
static int                      g_aos_blk_ready;
static int                      g_aos_blk_probed;
static int                      g_aos_blk_driver_ok;
static int                      g_aos_blk_pumped;
static int                      g_host_backend;
static int                      g_host_read_pumped;

#define AOS_HOST_BLK_EP 12u

static void aos_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0u) {
        *d++ = *s++;
    }
}

static uint32_t host_blk_call(uint32_t op, uint64_t sector, uint32_t count,
                              uint64_t *capacity)
{
    seL4_Word payload0 = (seL4_Word)op |
                         ((seL4_Word)(uint32_t)sector << 32);
    seL4_Word payload1 = (seL4_Word)(uint32_t)(sector >> 32) |
                         ((seL4_Word)count << 32);
    seL4_MessageInfo_t tag;
    seL4_MessageInfo_t reply;

    seL4_SetMR(0, op);
    seL4_SetMR(1, 16u);
    seL4_SetMR(2, payload0);
    seL4_SetMR(3, payload1);
    seL4_SetMR(4, 0u);
    seL4_SetMR(5, 0u);
    seL4_SetMR(6, 0u);
    seL4_SetMR(7, 0u);
    tag = seL4_MessageInfo_new(op, 0u, 0u, 8u);
    reply = seL4_Call(AOS_HOST_BLK_EP, tag);
    if (seL4_MessageInfo_get_length(reply) < 3u ||
        seL4_GetMR(1) < 4u) {
        return AOS_HOST_BLK_ERR_NODEV;
    }

    payload0 = seL4_GetMR(2);
    if (capacity && (uint32_t)payload0 == AOS_HOST_BLK_OK &&
        seL4_GetMR(1) >= 16u &&
        seL4_MessageInfo_get_length(reply) >= 4u) {
        payload1 = seL4_GetMR(3);
        *capacity = ((uint64_t)(uint32_t)payload1 << 32) |
                    (uint64_t)(uint32_t)(payload0 >> 32);
    }
    return (uint32_t)payload0;
}

static aos_blk_resp_status_t host_blk_backend(
    void *ctx, aos_blk_virt_client_t *client, const aos_blk_req_t *req)
{
    uint8_t *dma = (uint8_t *)(AGENTOS_BLK_SHARED_VA +
                               AGENTOS_BLK_SHARED_DMA_OFF);
    uint32_t nbytes = (uint32_t)req->count * AOS_BLK_TRANSFER_SIZE;
    uint64_t data_end = req->io_or_offset + (uint64_t)nbytes;
    uint64_t sector = req->block_number *
                      (AOS_BLK_TRANSFER_SIZE / AOS_HOST_BLK_SECTOR_SIZE);
    uint32_t sectors = (uint32_t)req->count *
                       (AOS_BLK_TRANSFER_SIZE / AOS_HOST_BLK_SECTOR_SIZE);
    uint32_t rc;
    (void)ctx;

    if (data_end > AOS_BLK_DATA_BYTES ||
        nbytes > AGENTOS_BLK_SHARED_DMA_SIZE) {
        return AOS_BLK_RESP_ERR_INVALID_PARAM;
    }

    switch (req->code) {
    case AOS_BLK_REQ_READ:
        rc = host_blk_call(AOS_HOST_BLK_OP_READ, sector, sectors, 0);
        if (rc == AOS_HOST_BLK_OK) {
            aos_copy(client->data + (uint32_t)req->io_or_offset,
                     dma, nbytes);
            if (!g_host_read_pumped) {
                g_host_read_pumped = 1;
                LOG_VMM("emulated virtio-blk: host-media read sector=%lu count=%u\n",
                        (unsigned long)sector, (unsigned)sectors);
            }
        }
        break;
    case AOS_BLK_REQ_WRITE:
        aos_copy(dma, client->data + (uint32_t)req->io_or_offset,
                 nbytes);
        rc = host_blk_call(AOS_HOST_BLK_OP_WRITE, sector, sectors, 0);
        break;
    case AOS_BLK_REQ_FLUSH:
    case AOS_BLK_REQ_BARRIER:
        rc = host_blk_call(AOS_HOST_BLK_OP_FLUSH, 0u, 0u, 0);
        break;
    default:
        return AOS_BLK_RESP_ERR_INVALID_PARAM;
    }

    if (rc == AOS_HOST_BLK_OK) {
        return AOS_BLK_RESP_OK;
    }
    if (rc == AOS_HOST_BLK_ERR_NODEV) {
        return AOS_BLK_RESP_ERR_NO_DEVICE;
    }
    if (rc == AOS_HOST_BLK_ERR_OOB) {
        return AOS_BLK_RESP_ERR_INVALID_PARAM;
    }
    return AOS_BLK_RESP_ERR_IO;
}

void aos_vmm_virtio_blk_init(void)
{
    uint8_t *region = g_blk_region;
    uint32_t i;
    uint64_t host_sectors = 0u;
    uint32_t host_info_rc;

    for (i = 0; i < AOS_BLK_SHMEM_SIZE; i++) {
        region[i] = 0;
    }

    aos_blk_virt_reset(&g_aos_virt);
    aos_blk_client_bind(region, 0u, &g_aos_client);
    aos_blk_client_init_queues(&g_aos_client);
    host_info_rc = host_blk_call(AOS_HOST_BLK_OP_INFO, 0u, 0u,
                                 &host_sectors);
    if (host_info_rc == AOS_HOST_BLK_OK &&
        host_sectors >=
            (AOS_BLK_TRANSFER_SIZE / AOS_HOST_BLK_SECTOR_SIZE)) {
        uint64_t host_blocks = host_sectors /
            (AOS_BLK_TRANSFER_SIZE / AOS_HOST_BLK_SECTOR_SIZE);
        if (host_blocks > UINT32_MAX) {
            host_blocks = UINT32_MAX;
        }
        aos_blk_storage_init(g_aos_client.info, (uint32_t)host_blocks);
        g_aos_client.info->read_only = true;
        aos_blk_virt_set_backend(&g_aos_virt, host_blk_backend, 0);
        g_host_backend = 1;
        LOG_VMM("emulated virtio-blk: agentOS host media ready sectors=%lu\n",
                (unsigned long)host_sectors);
    } else {
        LOG_VMM("emulated virtio-blk: host unavailable rc=%u; using RAM backend\n",
                (unsigned)host_info_rc);
        aos_blk_storage_init(g_aos_client.info, AOS_BLK_DISK_BLOCKS);
        aos_blk_virt_set_disk(&g_aos_virt,
                              region + AOS_BLK_DISK_OFF,
                              AOS_BLK_DISK_BLOCKS);
    }
    if (aos_blk_virt_add_client(&g_aos_virt, &g_aos_client) != 0) {
        LOG_VMM_ERR("emulated virtio-blk: add client failed\n");
        return;
    }

    blk_queue_init(&g_queue, (blk_req_queue_t *)g_aos_client.req,
                   (blk_resp_queue_t *)g_aos_client.resp, AOS_BLK_QUEUE_CAPACITY);

    /*
     * libvmm calls vmm_notify(server_ch) when the guest virtq is kicked.
     * server_ch is 0 (no blk_virt PD yet). Plug the request queue so
     * virtio_blk_mmio_queue_notify skips seL4_Signal(0). We pump after
     * every VM MMIO fault instead. vmm_notify also no-ops a 0 cap.
     */
    blk_queue_plug_req(&g_queue);

    if (!virtio_mmio_blk_init(&g_aos_blk,
                              AOS_VIRTIO_BLK_GUEST_IPA,
                              AOS_VIRTIO_BLK_MMIO_SIZE,
                              AOS_VIRTIO_BLK_VIRQ,
                              (uintptr_t)g_aos_client.data,
                              AOS_BLK_DATA_BYTES,
                              (blk_storage_info_t *)g_aos_client.info,
                              &g_queue,
                              AOS_BLK_QUEUE_CAPACITY,
                              0)) {
        LOG_VMM_ERR("emulated virtio-blk: virtio_mmio_blk_init failed\n");
        return;
    }

    g_aos_blk_ready = 1;
    LOG_VMM("emulated virtio-blk IPA 0x%lx IRQ %u (sDDF %s backend)\n",
            (unsigned long)AOS_VIRTIO_BLK_GUEST_IPA,
            (unsigned)AOS_VIRTIO_BLK_VIRQ,
            g_host_backend ? "agentOS host-media" : "RAM");
}

void aos_vmm_virtio_blk_after_fault(void)
{
    virtio_queue_handler_t *vq;
    uint32_t status;
    uint32_t n;

    if (!g_aos_blk_ready) {
        return;
    }

    status = g_aos_blk.virtio_device.regs.Status;
    if (!g_aos_blk_probed && (status & VIRTIO_CONFIG_S_ACKNOWLEDGE)) {
        g_aos_blk_probed = 1;
        LOG_VMM("emulated virtio-blk: guest probed IPA 0x%lx (status=0x%x)\n",
                (unsigned long)AOS_VIRTIO_BLK_GUEST_IPA, (unsigned)status);
    }
    if (!g_aos_blk_driver_ok && (status & VIRTIO_CONFIG_S_DRIVER_OK)) {
        g_aos_blk_driver_ok = 1;
        LOG_VMM("emulated virtio-blk: guest DRIVER_OK virq %u capacity %u blocks\n",
                (unsigned)AOS_VIRTIO_BLK_VIRQ,
                (unsigned)g_aos_client.info->capacity);
    }

    n = aos_blk_virt_pump(&g_aos_virt);
    if (!g_aos_blk_pumped && n > 0u) {
        g_aos_blk_pumped = 1;
        LOG_VMM("emulated virtio-blk: pumped %u request(s)\n", n);
    }

    vq = &g_aos_blk.virtio_device.vqs[VIRTIO_BLK_DEFAULT_VIRTQ];
    if (!vq->ready || vq->virtq.avail == NULL) {
        return;
    }

    (void)virtio_blk_handle_resp(&g_aos_blk);
    /* Keep plugged so a later handle_resp enqueue cannot unplug. */
    if (g_queue.req_queue) {
        g_queue.req_queue->plugged = true;
    }
}
