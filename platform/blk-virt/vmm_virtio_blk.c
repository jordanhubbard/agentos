/*
 * Guest-facing virtio-blk: libvmm device at AOS_VIRTIO_BLK_GUEST_IPA,
 * backend = sDDF queues + canonical agentOS block-service media.
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
static uint32_t                 g_media_id;
static uint32_t                 g_host_request_count;

#define AOS_HOST_BLK_EP 12u

static uint8_t *host_dma(void)
{
    return (uint8_t *)(AGENTOS_BLK_SHARED_VA +
                       AGENTOS_BLK_MEDIA_DMA_OFF(g_media_id) +
                       AGENTOS_BLK_SHARED_DMA_DATA_OFF);
}

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
    seL4_SetMR(1, 20u);
    seL4_SetMR(2, payload0);
    seL4_SetMR(3, payload1);
    seL4_SetMR(4, (seL4_Word)g_media_id);
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

static uint32_t host_blk_transfer(uint32_t op, uint64_t sector,
                                  uint8_t *client_data, uint32_t nbytes)
{
    uint8_t *dma = host_dma();
    uint32_t sectors_left = nbytes / AOS_HOST_BLK_SECTOR_SIZE;
    uint32_t byte_offset = 0u;

    while (sectors_left > 0u) {
        uint32_t sectors = sectors_left;
        uint32_t bytes;
        uint32_t rc;

        if (sectors > AGENTOS_BLK_SHARED_DMA_MAX_SECTORS) {
            sectors = AGENTOS_BLK_SHARED_DMA_MAX_SECTORS;
        }
        bytes = sectors * AOS_HOST_BLK_SECTOR_SIZE;
        if (op == AOS_HOST_BLK_OP_WRITE) {
            aos_copy(dma, client_data + byte_offset, bytes);
        }
        rc = host_blk_call(op, sector, sectors, 0);
        if (rc != AOS_HOST_BLK_OK) {
            return rc;
        }
        if (op == AOS_HOST_BLK_OP_READ) {
            aos_copy(client_data + byte_offset, dma, bytes);
        }
        sector += sectors;
        sectors_left -= sectors;
        byte_offset += bytes;
    }
    return AOS_HOST_BLK_OK;
}

#define ISO9660_SECTOR_SIZE 2048u

static uint8_t g_iso_sector[ISO9660_SECTOR_SIZE];

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool iso_read_sector(uint32_t lba)
{
    uint8_t *dma = host_dma();
    uint64_t host_sector =
        (uint64_t)lba * (ISO9660_SECTOR_SIZE / AOS_HOST_BLK_SECTOR_SIZE);
    uint32_t count = ISO9660_SECTOR_SIZE / AOS_HOST_BLK_SECTOR_SIZE;
    uint32_t rc = host_blk_call(AOS_HOST_BLK_OP_READ, host_sector, count, 0);
    if (rc != AOS_HOST_BLK_OK) {
        LOG_VMM_ERR("emulated virtio-blk: ISO sector read failed lba=%u rc=%u\n",
                    (unsigned)lba, (unsigned)rc);
        return false;
    }
    aos_copy(g_iso_sector, dma, ISO9660_SECTOR_SIZE);
    LOG_VMM("emulated virtio-blk: ISO sector=%u first=%x %x %x %x\n",
            (unsigned)lba, (unsigned)g_iso_sector[0],
            (unsigned)g_iso_sector[1], (unsigned)g_iso_sector[2],
            (unsigned)g_iso_sector[3]);
    return true;
}

static uint8_t ascii_fold(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + ('a' - 'A')) : c;
}

static bool iso_name_eq(const uint8_t *id, uint8_t id_len, const char *name)
{
    uint8_t n = 0u;
    while (name[n] != '\0') {
        n++;
    }
    if (id_len >= 2u && id[id_len - 2u] == ';' &&
        id[id_len - 1u] == '1') {
        id_len -= 2u;
    }
    if (id_len > 0u && id[id_len - 1u] == '.') {
        id_len--;
    }
    if (id_len != n) {
        return false;
    }
    for (uint8_t i = 0u; i < n; i++) {
        if (ascii_fold(id[i]) != ascii_fold((uint8_t)name[i])) {
            return false;
        }
    }
    return true;
}

static bool iso_find_entry(uint32_t dir_lba, uint32_t dir_size,
                           const char *name, uint32_t *entry_lba,
                           uint32_t *entry_size)
{
    uint32_t sectors =
        (dir_size + ISO9660_SECTOR_SIZE - 1u) / ISO9660_SECTOR_SIZE;
    for (uint32_t s = 0u; s < sectors; s++) {
        if (!iso_read_sector(dir_lba + s)) {
            return false;
        }
        uint32_t off = 0u;
        while (off < ISO9660_SECTOR_SIZE) {
            uint8_t record_len = g_iso_sector[off];
            if (record_len == 0u) {
                break;
            }
            if (off + record_len > ISO9660_SECTOR_SIZE ||
                record_len < 34u) {
                return false;
            }
            uint8_t id_len = g_iso_sector[off + 32u];
            if ((uint32_t)33u + id_len <= record_len &&
                iso_name_eq(&g_iso_sector[off + 33u], id_len, name)) {
                *entry_lba = read_le32(&g_iso_sector[off + 2u]);
                *entry_size = read_le32(&g_iso_sector[off + 10u]);
                return true;
            }
            off += record_len;
        }
    }
    return false;
}

bool aos_vmm_virtio_blk_load_casper_initrd(uintptr_t guest_dest,
                                           size_t guest_capacity,
                                           size_t *loaded_size)
{
    uint32_t root_lba;
    uint32_t root_size;
    uint32_t casper_lba;
    uint32_t casper_size;
    uint32_t initrd_lba;
    uint32_t initrd_size;
    uint8_t *dma = host_dma();

    if (!g_host_backend || !iso_read_sector(16u)) {
        LOG_VMM_ERR("emulated virtio-blk: failed to read ISO9660 primary descriptor\n");
        return false;
    }
    LOG_VMM("emulated virtio-blk: ISO PVD bytes=%x %x %x %x %x %x %x %x\n",
            (unsigned)g_iso_sector[0], (unsigned)g_iso_sector[1],
            (unsigned)g_iso_sector[2], (unsigned)g_iso_sector[3],
            (unsigned)g_iso_sector[4], (unsigned)g_iso_sector[5],
            (unsigned)g_iso_sector[6], (unsigned)g_iso_sector[7]);
    if (g_iso_sector[0] != 1u ||
        g_iso_sector[1] != 'C' || g_iso_sector[2] != 'D' ||
        g_iso_sector[3] != '0' || g_iso_sector[4] != '0' ||
        g_iso_sector[5] != '1') {
        LOG_VMM_ERR("emulated virtio-blk: invalid ISO9660 primary descriptor\n");
        return false;
    }
    root_lba = read_le32(&g_iso_sector[158u]);
    root_size = read_le32(&g_iso_sector[166u]);
    LOG_VMM("emulated virtio-blk: ISO root lba=%u bytes=%u\n",
            (unsigned)root_lba, (unsigned)root_size);
    if (!iso_find_entry(root_lba, root_size, "casper",
                        &casper_lba, &casper_size)) {
        LOG_VMM_ERR("emulated virtio-blk: ISO /casper not found\n");
        return false;
    }
    LOG_VMM("emulated virtio-blk: ISO casper lba=%u bytes=%u\n",
            (unsigned)casper_lba, (unsigned)casper_size);
    if (!iso_find_entry(casper_lba, casper_size, "initrd",
                        &initrd_lba, &initrd_size)) {
        LOG_VMM_ERR("emulated virtio-blk: ISO /casper/initrd not found\n");
        return false;
    }
    if (initrd_size == 0u || (size_t)initrd_size > guest_capacity) {
        LOG_VMM_ERR("emulated virtio-blk: casper initrd size invalid\n");
        return false;
    }

    size_t copied = 0u;
    while (copied < initrd_size) {
        size_t remaining = (size_t)initrd_size - copied;
        uint32_t bytes = remaining > AGENTOS_BLK_SHARED_DMA_DATA_SIZE
            ? AGENTOS_BLK_SHARED_DMA_DATA_SIZE : (uint32_t)remaining;
        uint32_t sectors =
            (bytes + AOS_HOST_BLK_SECTOR_SIZE - 1u) /
            AOS_HOST_BLK_SECTOR_SIZE;
        uint64_t host_sector =
            (uint64_t)initrd_lba *
            (ISO9660_SECTOR_SIZE / AOS_HOST_BLK_SECTOR_SIZE) +
            copied / AOS_HOST_BLK_SECTOR_SIZE;
        if (host_blk_call(AOS_HOST_BLK_OP_READ, host_sector, sectors, 0) !=
            AOS_HOST_BLK_OK) {
            return false;
        }
        volatile uint8_t *dest =
            (volatile uint8_t *)(guest_dest + copied);
        for (uint32_t i = 0u; i < bytes; i++) {
            dest[i] = dma[i];
        }
        copied += bytes;
    }

    g_host_read_pumped = 1;
    uint32_t hash = 2166136261u;
    const volatile uint8_t *image = (const volatile uint8_t *)guest_dest;
    for (uint32_t i = 0u; i < initrd_size; i++) {
        hash = (hash ^ image[i]) * 16777619u;
    }
    LOG_VMM("emulated virtio-blk: loaded casper/initrd from host media bytes=%u fnv1a=%x\n",
            (unsigned)initrd_size, (unsigned)hash);
    LOG_VMM("emulated virtio-blk: host-media read sector=%lu count=%u\n",
            (unsigned long)((uint64_t)initrd_lba * 4u),
            (unsigned)((initrd_size + 511u) / 512u));
    if (loaded_size) {
        *loaded_size = initrd_size;
    }
    return true;
}

static aos_blk_resp_status_t host_blk_backend(
    void *ctx, aos_blk_virt_client_t *client, const aos_blk_req_t *req)
{
    uint32_t nbytes = (uint32_t)req->count * AOS_BLK_TRANSFER_SIZE;
    uint64_t data_end = req->io_or_offset + (uint64_t)nbytes;
    uint64_t sector = req->block_number *
                      (AOS_BLK_TRANSFER_SIZE / AOS_HOST_BLK_SECTOR_SIZE);
    uint32_t sectors = (uint32_t)req->count *
                       (AOS_BLK_TRANSFER_SIZE / AOS_HOST_BLK_SECTOR_SIZE);
    uint32_t rc;
    (void)ctx;

    g_host_request_count++;
    if (g_host_request_count <= 16u ||
        (g_host_request_count & (g_host_request_count - 1u)) == 0u) {
        LOG_VMM("emulated virtio-blk: media=%u request=%u code=%u block=%lu count=%u offset=%lu\n",
                (unsigned)g_media_id, (unsigned)g_host_request_count,
                (unsigned)req->code, (unsigned long)req->block_number,
                (unsigned)req->count, (unsigned long)req->io_or_offset);
    }
    if (data_end > AOS_BLK_DATA_BYTES) {
        LOG_VMM_ERR("emulated virtio-blk: request exceeds staging region bytes=%u end=%lu\n",
                    (unsigned)nbytes, (unsigned long)data_end);
        return AOS_BLK_RESP_ERR_INVALID_PARAM;
    }

    switch (req->code) {
    case AOS_BLK_REQ_READ:
        rc = host_blk_transfer(
            AOS_HOST_BLK_OP_READ, sector,
            client->data + (uint32_t)req->io_or_offset, nbytes);
        if (rc == AOS_HOST_BLK_OK) {
            if (!g_host_read_pumped) {
                g_host_read_pumped = 1;
                LOG_VMM("emulated virtio-blk: host-media read sector=%lu count=%u\n",
                        (unsigned long)sector, (unsigned)sectors);
            }
        }
        break;
    case AOS_BLK_REQ_WRITE:
        rc = host_blk_transfer(
            AOS_HOST_BLK_OP_WRITE, sector,
            client->data + (uint32_t)req->io_or_offset, nbytes);
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
    LOG_VMM_ERR("emulated virtio-blk: host request failed media=%u rc=%u\n",
                (unsigned)g_media_id, (unsigned)rc);
    if (rc == AOS_HOST_BLK_ERR_NODEV) {
        return AOS_BLK_RESP_ERR_NO_DEVICE;
    }
    if (rc == AOS_HOST_BLK_ERR_OOB) {
        return AOS_BLK_RESP_ERR_INVALID_PARAM;
    }
    return AOS_BLK_RESP_ERR_IO;
}

void aos_vmm_virtio_blk_init(uint32_t media_id)
{
    uint8_t *region = g_blk_region;
    uint32_t i;
    uint64_t host_sectors = 0u;
    uint32_t host_info_rc;

    if (media_id >= AOS_HOST_BLK_MEDIA_COUNT) {
        LOG_VMM_ERR("emulated virtio-blk: invalid host media %u\n",
                    (unsigned)media_id);
        return;
    }
    g_media_id = media_id;
    g_host_request_count = 0u;

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
        /*
         * ISO9660 requires a logical sector no larger than 2048 bytes.
         * The backend still batches requests through 4 KiB sDDF transfer
         * windows, but the guest-visible VirtIO geometry is 512-byte sectors.
         */
        g_aos_client.info->sector_size = AOS_HOST_BLK_SECTOR_SIZE;
        g_aos_client.info->block_size = 0u;
        aos_blk_virt_set_backend(&g_aos_virt, host_blk_backend, 0);
        g_host_backend = 1;
        LOG_VMM("emulated virtio-blk: agentOS host media %u ready sectors=%lu\n",
                (unsigned)g_media_id,
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
    /*
     * FreeBSD arm64 rejects VIRTIO_BLK_F_SIZE_MAX values below MAXPHYS.
     * The extra transfer cell accommodates a MAXPHYS request beginning at a
     * non-4K sector; host IPC then chunks it through the bounded DMA window.
     */
    g_aos_blk.config.size_max = AOS_BLK_GUEST_MAX_SEGMENT_SIZE;

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
    uint32_t rounds = 0u;
    uint32_t total = 0u;
    bool pending;

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

    vq = &g_aos_blk.virtio_device.vqs[VIRTIO_BLK_DEFAULT_VIRTQ];
    if (!vq->ready || vq->virtq.avail == NULL) {
        return;
    }

    /*
     * This backend is synchronous and has no blk_virt notification cap.
     * handle_resp() can consume additional guest descriptors and enqueue more
     * sDDF requests after the pump has run, so drain request/response batches
     * until no new work remains.
     */
    do {
        n = aos_blk_virt_pump(&g_aos_virt);
        total += n;
        if (!g_aos_blk_pumped && n > 0u) {
            g_aos_blk_pumped = 1;
            LOG_VMM("emulated virtio-blk: pumped %u request(s)\n", n);
        }
        (void)virtio_blk_handle_resp(&g_aos_blk);
        /* Keep plugged so a later handle_resp enqueue cannot signal cap 0. */
        if (g_queue.req_queue) {
            g_queue.req_queue->plugged = true;
        }
        pending = !blk_queue_empty_req(&g_queue) ||
                  !blk_queue_empty_resp(&g_queue);
        rounds++;
    } while ((n > 0u || pending) &&
             rounds <= (AOS_BLK_QUEUE_CAPACITY * 2u + 1u));

    if (n > 0u || pending) {
        LOG_VMM_ERR("emulated virtio-blk: synchronous drain did not quiesce\n");
    }
    if (total > 0u) {
        static uint32_t drain_count;
        drain_count++;
        if (drain_count <= 16u ||
            (drain_count & (drain_count - 1u)) == 0u) {
            LOG_VMM("emulated virtio-blk: drain=%u rounds=%u requests=%u avail=%u last=%u used=%u irq=0x%x\n",
                    (unsigned)drain_count, (unsigned)rounds, (unsigned)total,
                    (unsigned)vq->virtq.avail->idx, (unsigned)vq->last_idx,
                    (unsigned)vq->virtq.used->idx,
                    (unsigned)g_aos_blk.virtio_device.regs.InterruptStatus);
        }
    }
}
