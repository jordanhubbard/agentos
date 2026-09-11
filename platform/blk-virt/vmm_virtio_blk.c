/*
 * Guest-facing virtio-blk: libvmm device at AOS_VIRTIO_BLK_GUEST_IPA,
 * backend = sDDF guest queues in the shared block region, serviced by the
 * blk_virt PD (platform/blk-virt/blk_virt.c).  The VMM never moves a block
 * request over IPC: it enqueues/dequeues the shared queues and exchanges
 * notifications with blk_virt (contracts/blk_virt_contract.h).  virtio_blk
 * alone owns the QEMU bus.8 transport and the bounded DMA window; this file
 * holds no virtio_blk endpoint and maps no DMA window.
 */

#include <contracts/blk_virt_contract.h>
#include "sel4_ipc.h"
#include "system_desc.h"
#include <libvmm/libvmm.h>
#include <libvmm/arch/aarch64/vgic/vgic.h>
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

/* This VMM owns one client stride of the shared block region. */
#if defined(AGENTOS_GUEST_SECONDARY)
#define AOS_BLK_VMM_CLIENT   1u
#define AOS_BLK_VMM_SLOT     BLK_VIRT_VMM_SLOT_SECONDARY
#else
#define AOS_BLK_VMM_CLIENT   0u
#define AOS_BLK_VMM_SLOT     BLK_VIRT_VMM_SLOT_PRIMARY
#endif

static struct virtio_blk_device g_aos_blk;
static aos_blk_virt_client_t    g_aos_client;
static blk_queue_handle_t       g_queue;
static int                      g_aos_blk_ready;
static int                      g_aos_blk_probed;
static int                      g_aos_blk_driver_ok;
static int                      g_aos_blk_pumped;
static int                      g_blk_virt_attached;
static uint32_t                 g_blk_virt_hw;
static uint32_t                 g_media_id;
static uint32_t                 g_resp_total;
static uint32_t                 g_drain_count;

static uint32_t blk_rd32(const uint8_t *p, uint32_t off)
{
    return (uint32_t)p[off] |
           ((uint32_t)p[off + 1u] << 8) |
           ((uint32_t)p[off + 2u] << 16) |
           ((uint32_t)p[off + 3u] << 24);
}

static void blk_wr32(uint8_t *p, uint32_t off, uint32_t value)
{
    p[off] = (uint8_t)value;
    p[off + 1u] = (uint8_t)(value >> 8);
    p[off + 2u] = (uint8_t)(value >> 16);
    p[off + 3u] = (uint8_t)(value >> 24);
}

static void aos_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0u) {
        *d++ = *s++;
    }
}

static void blk_fence(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* ── blk_virt control + notifications ───────────────────────────────────── */

static void blk_virt_attach(uint32_t media_id)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    uint32_t status;

    req.opcode = BLK_VIRT_OP_ATTACH;
    req.length = (uint32_t)sizeof(blk_virt_attach_req_t);
    blk_wr32(req.data, 0u, BLK_VIRT_CONTRACT_VERSION);
    blk_wr32(req.data, 4u, AOS_BLK_VMM_CLIENT);
    blk_wr32(req.data, 8u, AOS_BLK_VMM_SLOT);
    blk_wr32(req.data, 12u, media_id);
    sel4_call((seL4_CPtr)PD_CNODE_SLOT_BLK_VIRT_EP, &req, &rep);
    status = blk_rd32(rep.data, 0u);
    if (rep.opcode != SEL4_ERR_OK || status != BLK_VIRT_OK ||
        rep.length < sizeof(blk_virt_attach_reply_t)) {
        LOG_VMM_ERR("emulated virtio-blk: blk_virt ATTACH failed rc=%u status=%u\n",
                    (unsigned)rep.opcode, (unsigned)status);
        return;
    }
    g_blk_virt_hw = blk_rd32(rep.data, 8u);
    g_blk_virt_attached = 1;
    blk_fence();
    LOG_VMM("emulated virtio-blk: attached to blk_virt contract v%u client %u media %u hw=%u capacity %lu blocks\n",
            (unsigned)blk_rd32(rep.data, 4u), (unsigned)AOS_BLK_VMM_CLIENT,
            (unsigned)media_id, (unsigned)g_blk_virt_hw,
            (unsigned long)g_aos_client.info->capacity);
}

static void blk_virt_kick(void)
{
    seL4_NBSend((seL4_CPtr)PD_CNODE_SLOT_BLK_VIRT_EP,
                seL4_MessageInfo_new(BLK_VIRT_EVENT_KICK, 0u, 0u, 0u));
}

/* Kick only while blk_virt asked for kicks (contract: it owns the word). */
static void blk_virt_kick_if_pending(void)
{
    blk_fence();
    if (!blk_queue_empty_req(&g_queue) &&
        *(volatile uint32_t *)&g_aos_client.signal->req_consumer_signalled == 0u) {
        blk_virt_kick();
    }
}

/*
 * Service the shared queues against blk_virt.  Called after every guest
 * exit and on every RESP_READY event:
 *   - complete the responses blk_virt queued (libvmm writes READ payloads
 *     into guest RAM through the GPA translation API and injects the virq);
 *   - kick blk_virt if the guest (or handle_resp's read-modify-write path)
 *     queued requests and blk_virt asked for kicks.
 * NBSend kicks are lossy; the word stays 0 until blk_virt drains, so a lost
 * kick is repeated on the next exit.
 */
static void blk_virt_service(const char *how)
{
    virtio_queue_handler_t *vq;
    uint32_t resp_n;

    if (!g_blk_virt_attached) {
        return;
    }

    blk_fence();
    resp_n = blk_queue_length_resp(&g_queue);
    if (resp_n > 0u) {
        g_resp_total += resp_n;
        (void)virtio_blk_handle_resp(&g_aos_blk);
        g_drain_count++;
        if (!g_aos_blk_pumped) {
            g_aos_blk_pumped = 1;
            LOG_VMM("emulated virtio-blk: pumped %u response(s) via blk_virt (%s)\n",
                    (unsigned)resp_n, how);
        }
        vq = &g_aos_blk.virtio_device.vqs[VIRTIO_BLK_DEFAULT_VIRTQ];
        if (g_drain_count <= 16u ||
            (g_drain_count & (g_drain_count - 1u)) == 0u) {
            LOG_VMM("emulated virtio-blk: drain=%u responses=%u total=%u avail=%u last=%u used=%u irq=0x%x\n",
                    (unsigned)g_drain_count, (unsigned)resp_n,
                    (unsigned)g_resp_total,
                    vq->virtq.avail ? (unsigned)vq->virtq.avail->idx : 0u,
                    (unsigned)vq->last_idx,
                    vq->virtq.used ? (unsigned)vq->virtq.used->idx : 0u,
                    (unsigned)g_aos_blk.virtio_device.regs.InterruptStatus);
        }
    }
    blk_virt_kick_if_pending();
}

void aos_vmm_virtio_blk_resp_ready(void)
{
    if (!g_aos_blk_ready) {
        return;
    }
    blk_virt_service("RESP_READY");
}

/* ── pre-boot media staging: the VMM as its own sDDF client ─────────────── */

/*
 * Synchronous READ of `count` transfer units at `block` into data cell 0.
 * Only valid before the guest owns the queues (no libvmm request in
 * flight), which is when the profile initrd is staged.  `wait` blocks the
 * VMM on its listen endpoint until an event arrives; blk_virt runs below
 * the VMM, so spinning would starve it.
 */
static bool vmm_blk_read_blocks(uint64_t block, uint16_t count,
                                aos_vmm_blk_wait_fn wait)
{
    blk_resp_status_t status = BLK_RESP_ERR_UNSPEC;
    uint16_t success_count = 0u;
    uint32_t id = 0u;

    if (!g_blk_virt_attached || wait == NULL || count == 0u ||
        count > AOS_BLK_DATA_CELLS ||
        !blk_queue_empty_req(&g_queue) || !blk_queue_empty_resp(&g_queue)) {
        return false;
    }
    if (blk_enqueue_req(&g_queue, BLK_REQ_READ, 0u, block, count, 0u) != 0) {
        return false;
    }
    blk_fence();
    blk_virt_kick();
    while (blk_queue_empty_resp(&g_queue)) {
        wait();
        blk_fence();
    }
    if (blk_dequeue_resp(&g_queue, &status, &success_count, &id) != 0) {
        return false;
    }
    blk_fence();
    return status == BLK_RESP_OK && success_count == count && id == 0u;
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

static bool iso_read_sector(uint32_t lba, aos_vmm_blk_wait_fn wait)
{
    uint64_t byte_off = (uint64_t)lba * ISO9660_SECTOR_SIZE;
    uint64_t block = byte_off / AOS_BLK_TRANSFER_SIZE;
    uint32_t in_block = (uint32_t)(byte_off % AOS_BLK_TRANSFER_SIZE);

    if (!vmm_blk_read_blocks(block, 1u, wait)) {
        LOG_VMM_ERR("emulated virtio-blk: ISO sector read failed lba=%u\n",
                    (unsigned)lba);
        return false;
    }
    aos_copy(g_iso_sector, g_aos_client.data + in_block, ISO9660_SECTOR_SIZE);
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
                           uint32_t *entry_size, uint8_t *entry_flags,
                           aos_vmm_blk_wait_fn wait)
{
    uint32_t sectors =
        (dir_size + ISO9660_SECTOR_SIZE - 1u) / ISO9660_SECTOR_SIZE;
    for (uint32_t s = 0u; s < sectors; s++) {
        if (!iso_read_sector(dir_lba + s, wait)) {
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
                *entry_flags = g_iso_sector[off + 25u];
                return true;
            }
            off += record_len;
        }
    }
    return false;
}

bool aos_vmm_virtio_blk_load_iso_file(const char *path,
                                      uintptr_t guest_dest,
                                      size_t guest_capacity,
                                      size_t *loaded_size,
                                      aos_vmm_blk_wait_fn wait)
{
    uint32_t root_lba;
    uint32_t root_size;
    uint32_t file_lba;
    uint32_t file_size;
    uint8_t file_flags;

    if (!g_blk_virt_attached || g_blk_virt_hw != BLK_VIRT_HW_VIRTIO_BLK ||
        !iso_read_sector(16u, wait)) {
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

    if (path == NULL || path[0] == '\0' || path[0] == '/') {
        LOG_VMM_ERR("emulated virtio-blk: invalid ISO file path\n");
        return false;
    }
    const char *cursor = path;
    uint32_t parent_lba = root_lba;
    uint32_t parent_size = root_size;
    for (;;) {
        char component[64];
        uint32_t length = 0u;
        while (*cursor != '\0' && *cursor != '/') {
            if (length >= sizeof(component) - 1u) {
                LOG_VMM_ERR("emulated virtio-blk: ISO path component too long\n");
                return false;
            }
            component[length++] = *cursor++;
        }
        component[length] = '\0';
        if (length == 0u ||
            (length == 1u && component[0] == '.') ||
            (length == 2u && component[0] == '.' && component[1] == '.')) {
            LOG_VMM_ERR("emulated virtio-blk: invalid ISO path component\n");
            return false;
        }
        if (!iso_find_entry(parent_lba, parent_size, component,
                            &file_lba, &file_size, &file_flags, wait)) {
            LOG_VMM_ERR("emulated virtio-blk: ISO path component not found\n");
            return false;
        }
        if (*cursor == '\0') {
            if ((file_flags & 2u) != 0u) {
                LOG_VMM_ERR("emulated virtio-blk: ISO path names a directory\n");
                return false;
            }
            break;
        }
        if ((file_flags & 2u) == 0u) {
            LOG_VMM_ERR("emulated virtio-blk: non-directory in ISO path\n");
            return false;
        }
        cursor++;
        if (*cursor == '\0') {
            LOG_VMM_ERR("emulated virtio-blk: trailing slash in ISO path\n");
            return false;
        }
        parent_lba = file_lba;
        parent_size = file_size;
    }
    if (file_size == 0u || (size_t)file_size > guest_capacity) {
        LOG_VMM_ERR("emulated virtio-blk: ISO file size invalid\n");
        return false;
    }

    /*
     * Stream the file through this client's data cells in bounded chunks:
     * each READ covers the transfer units spanning [abs, abs + bytes), at
     * most AOS_BLK_GUEST_MAX_SEGMENT_SIZE + one partial unit on either side,
     * which is exactly what AOS_BLK_DATA_CELLS provides.
     */
    uint64_t file_off = (uint64_t)file_lba * ISO9660_SECTOR_SIZE;
    size_t copied = 0u;
    uint32_t chunks = 0u;
    while (copied < file_size) {
        size_t remaining = (size_t)file_size - copied;
        uint32_t bytes = remaining > (AOS_BLK_GUEST_MAX_SEGMENT_SIZE - AOS_BLK_TRANSFER_SIZE)
            ? (AOS_BLK_GUEST_MAX_SEGMENT_SIZE - AOS_BLK_TRANSFER_SIZE)
            : (uint32_t)remaining;
        uint64_t abs = file_off + copied;
        uint64_t first_block = abs / AOS_BLK_TRANSFER_SIZE;
        uint64_t end_block = (abs + bytes + AOS_BLK_TRANSFER_SIZE - 1u) /
                             AOS_BLK_TRANSFER_SIZE;
        uint16_t count = (uint16_t)(end_block - first_block);

        if (!vmm_blk_read_blocks(first_block, count, wait)) {
            LOG_VMM_ERR("emulated virtio-blk: ISO file read failed block=%lu count=%u\n",
                        (unsigned long)first_block, (unsigned)count);
            return false;
        }
        aos_copy((void *)(guest_dest + copied),
                 g_aos_client.data +
                     (uint32_t)(abs - first_block * AOS_BLK_TRANSFER_SIZE),
                 bytes);
        copied += bytes;
        chunks++;
        if ((chunks & (chunks - 1u)) == 0u) {
            LOG_VMM("emulated virtio-blk: ISO file staging chunks=%u bytes=%u\n",
                    (unsigned)chunks, (unsigned)copied);
        }
    }

    blk_fence();
    LOG_VMM("emulated virtio-blk: loaded profile ISO file bytes=%u\n",
            (unsigned)file_size);
    LOG_VMM("emulated virtio-blk: staged ISO file via blk_virt sector=%lu count=%u\n",
            (unsigned long)((uint64_t)file_lba * 4u),
            (unsigned)((file_size + 511u) / 512u));
    if (loaded_size) {
        *loaded_size = file_size;
    }
    return true;
}

/* ── device bring-up ────────────────────────────────────────────────────── */

void aos_vmm_virtio_blk_init(uint32_t media_id)
{
    uint8_t *region = (uint8_t *)AOS_BLK_SHMEM_VA;

    if (media_id >= AOS_HOST_BLK_MEDIA_COUNT) {
        LOG_VMM_ERR("emulated virtio-blk: invalid host media %u\n",
                    (unsigned)media_id);
        return;
    }
    g_media_id = media_id;

    aos_blk_client_bind(region, AOS_BLK_VMM_CLIENT, &g_aos_client);
    aos_blk_client_init_queues(&g_aos_client);

    /*
     * Queues are zeroed (above) before blk_virt binds them.  blk_virt fills
     * storage_info from the host media (or its RAM disk) during ATTACH, so
     * libvmm sees the final geometry below.
     */
    blk_virt_attach(media_id);
    if (!g_blk_virt_attached) {
        LOG_VMM_ERR("emulated virtio-blk: no virtualizer; device not created\n");
        return;
    }

    blk_queue_init(&g_queue, (blk_req_queue_t *)g_aos_client.req,
                   (blk_resp_queue_t *)g_aos_client.resp, AOS_BLK_QUEUE_CAPACITY);

    /*
     * server_ch stays 0: libvmm does not signal on enqueue; the kick is
     * issued from blk_virt_service() under the contract's
     * req_consumer_signalled rule after every guest exit.
     */
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
    /* The extra transfer cell accommodates a maximum-size request beginning
     * at a non-4K sector; blk_virt chunks it through the driver's window. */
    g_aos_blk.config.size_max = AOS_BLK_GUEST_MAX_SEGMENT_SIZE;

    g_aos_blk_ready = 1;
    LOG_VMM("emulated virtio-blk IPA 0x%lx IRQ %u (sDDF queues to blk_virt, not QEMU; %s media)\n",
            (unsigned long)AOS_VIRTIO_BLK_GUEST_IPA,
            (unsigned)AOS_VIRTIO_BLK_VIRQ,
            g_blk_virt_hw == BLK_VIRT_HW_VIRTIO_BLK ? "host" : "RAM");
}

void aos_vmm_virtio_blk_after_fault(void)
{
    uint32_t status;

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

    blk_virt_service("after guest exit");
}
