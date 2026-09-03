/*
 * Guest-facing virtio-blk: libvmm device at AOS_VIRTIO_BLK_GUEST_IPA,
 * backend = sDDF queues + local aos_blk_virt_pump (RAM disk until blk_drv).
 *
 * QEMU virtio-mmio blk at 0x0A000200 remains a kill-dated passthrough
 * crutch (guest vda). This device is not in the live DTB this pass.
 */

#include <libvmm/libvmm.h>
#include <libvmm/virtio/block.h>
#include <sddf/blk/queue.h>
#include <sddf/blk/storage_info.h>
#include <platform/blk_layout.h>
#include <platform/blk_virt_pump.h>
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

void aos_vmm_virtio_blk_init(void)
{
    uint8_t *region = g_blk_region;
    uint32_t i;

    for (i = 0; i < AOS_BLK_SHMEM_SIZE; i++) {
        region[i] = 0;
    }

    aos_blk_virt_reset(&g_aos_virt);
    aos_blk_client_bind(region, 0u, &g_aos_client);
    aos_blk_client_init_queues(&g_aos_client);
    aos_blk_storage_init(g_aos_client.info, AOS_BLK_DISK_BLOCKS);
    aos_blk_virt_set_disk(&g_aos_virt, region + AOS_BLK_DISK_OFF, AOS_BLK_DISK_BLOCKS);
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
    LOG_VMM("emulated virtio-blk IPA 0x%lx IRQ %u (sDDF RAM pump, not QEMU)\n",
            (unsigned long)AOS_VIRTIO_BLK_GUEST_IPA,
            (unsigned)AOS_VIRTIO_BLK_VIRQ);
}

void aos_vmm_virtio_blk_after_fault(void)
{
    virtio_queue_handler_t *vq;

    if (!g_aos_blk_ready) {
        return;
    }
    (void)aos_blk_virt_pump(&g_aos_virt);

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
