/*
 * virtio_blk.c — virtio-blk block device driver protection domain for agentOS
 *
 * ── Physical address assumption ─────────────────────────────────────────────
 * virtio-MMIO requires the PHYSICAL addresses of virtqueue memory regions to
 * be written into the QueueDesc/QueueAvail/QueueUsed MMIO registers.
 *
 * Virtual and physical addresses are distinct. The root task maps the host
 * device page at AGENTOS_HOST_BLK_MMIO_VA and maps one shared large frame at
 * AGENTOS_BLK_SHARED_VA. Its physical base is carried in frame metadata.
 *
 * Queue and DMA memory live in a 2 MiB frame shared with blk_virt. The root
 * task records the frame's physical address in its metadata, so this driver
 * never assumes that a virtual address is also a DMA address.
 * ────────────────────────────────────────────────────────────────────────────
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "virtio_blk.h"
#include "arch_barrier.h"
#include <platform/blk_host_layout.h>
#include <platform/virtio_host_transport.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * Microkit setvar_vaddr symbols — written by the Microkit runtime before init()
 * ──────────────────────────────────────────────────────────────────────────── */

/* Host device MMIO VA installed by the root task. */
uintptr_t blk_mmio_vaddr;

/* Shared DMA VA installed by the root task. */
uintptr_t blk_dma_shmem_vaddr;

/* log_drain_rings_vaddr required by log_drain_write() in agentos.h */
uintptr_t log_drain_rings_vaddr;

/* ─────────────────────────────────────────────────────────────────────────────
 * Device state
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    bool               initialized;
    aos_virtio_host_t   transport;   /* driver-owned MMIO or modern PCI */
    uint32_t           media_id;      /* BLK_MEDIA_* */
    uint32_t           queue_off;     /* offset in shared large frame */
    uint32_t           dma_off;       /* offset in shared large frame */
    uint64_t           capacity;     /* total 512-byte sectors on the device */
    uint32_t           block_size;   /* logical block size reported by device (bytes) */
    bool               read_only;    /* VIRTIO_BLK_F_RO advertised by the medium */
    uint32_t           error_count;  /* cumulative I/O errors since boot */
    uint32_t           init_error;   /* non-zero initialization stage */
} blk_device_t;

static blk_device_t dev[AOS_HOST_BLK_MEDIA_COUNT];
static uint64_t g_blk_shared_paddr;

/* ─────────────────────────────────────────────────────────────────────────────
 * Virtqueue memory
 *
 * For the MVP we use a single queue of depth 1 (one descriptor chain in
 * flight at a time, polled synchronously).  The three virtqueue regions
 * are physically contiguous inside the shared large frame and their physical
 * addresses are passed to the device.
 *
 * Layout inside queue_mem[]:
 *   [0]                    : descriptor table  (1 × 16 bytes = 16 bytes)
 *   [AVAIL_OFFSET]         : available ring    (4 + 2×1 bytes = 6 bytes, aligned 2)
 *   [USED_OFFSET]          : used ring         (4 + 8×1 bytes = 12 bytes, aligned 4)
 *
 * The queue layout reserves one 4 KB page.
 * ──────────────────────────────────────────────────────────────────────────── */

/* Offsets within queue_mem for each virtqueue region */
#define DESC_OFFSET     0u
#define AVAIL_OFFSET    0x100u
#define USED_OFFSET     0x200u

static volatile uint8_t *queue_mem(const blk_device_t *device)
{
    return (volatile uint8_t *)(AGENTOS_BLK_SHARED_VA + device->queue_off);
}

static volatile virtq_desc_t *queue_desc(const blk_device_t *device)
{
    return (volatile virtq_desc_t *)(queue_mem(device) + DESC_OFFSET);
}

static volatile virtq_avail_t *queue_avail(const blk_device_t *device)
{
    return (volatile virtq_avail_t *)(queue_mem(device) + AVAIL_OFFSET);
}

static volatile virtq_used_t *queue_used(const blk_device_t *device)
{
    return (volatile virtq_used_t *)(queue_mem(device) + USED_OFFSET);
}

static uint64_t queue_paddr(const blk_device_t *device)
{
    return g_blk_shared_paddr + device->queue_off;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Request scratch buffers (inside the DMA window)
 *
 * The virtio-blk request consists of:
 *   [0..15]  : virtio_blk_req_hdr_t  (16 bytes, driver→device)
 *   [16..N]  : data payload           (sector_count × 512 bytes, rd=device→driver / wr=driver→device)
 *   [last 1] : status byte            (device→driver)
 *
 * We lay these out inside blk_dma_shmem so that the device can DMA all three
 * regions directly.  The header sits at shmem+0, the data at shmem+16, and
 * the status byte at shmem+16+data_len.
 *
 * The common layout assigns each configured medium its own staging window.
 * Per-medium limits keep data and status inside the selected window.
 * ──────────────────────────────────────────────────────────────────────────── */

#define DMA_HDR_OFFSET          0u
#define DMA_DATA_OFFSET         AGENTOS_BLK_SHARED_DMA_DATA_OFF
#define DMA_STATUS_OFFSET(cnt)  (DMA_DATA_OFFSET + (uint32_t)(cnt) * 512u)

static volatile uint8_t *dma_base(const blk_device_t *device)
{
    return (volatile uint8_t *)(AGENTOS_BLK_SHARED_VA + device->dma_off);
}

static uint64_t dma_paddr(const blk_device_t *device)
{
    return g_blk_shared_paddr + device->dma_off;
}

static uint32_t dma_max_sectors(const blk_device_t *device)
{
    return AGENTOS_BLK_MEDIA_DMA_MAX_SECTORS(device->media_id);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Internal: perform a virtio-blk I/O request (synchronous polling)
 *
 * type   : VIRTIO_BLK_T_IN (read), VIRTIO_BLK_T_OUT (write), or
 *          VIRTIO_BLK_T_FLUSH
 * sector : starting LBA (ignored for flush)
 * count  : number of 512-byte sectors (0 for flush)
 *
 * The request header and status byte live in blk_dma_shmem at fixed offsets.
 * For reads and writes, the data region also lives in blk_dma_shmem (VFS
 * server shares the same physical window via a different vaddr mapping).
 *
 * Returns BLK_OK on success, BLK_ERR_IO on device error or timeout.
 * ──────────────────────────────────────────────────────────────────────────── */
static uint32_t virtio_blk_do_io(blk_device_t *device, uint32_t type,
                                 uint64_t sector, uint32_t count)
{
    uint32_t data_len    = count * 512u;
    uint32_t status_off  = DMA_STATUS_OFFSET(count);
    volatile uint8_t *dma = dma_base(device);
    uint64_t dma_pa = dma_paddr(device);
    volatile virtq_desc_t *desc = queue_desc(device);
    volatile virtq_avail_t *avail = queue_avail(device);
    volatile virtq_used_t *used = queue_used(device);

    /* ── Step 1: Write request header into DMA shmem ── */
    volatile virtio_blk_req_hdr_t *hdr =
        (volatile virtio_blk_req_hdr_t *)(dma + DMA_HDR_OFFSET);
    hdr->type     = type;
    hdr->reserved = 0;
    hdr->sector   = sector;

    /* Initialise the status byte to a non-zero sentinel so we can detect
     * whether the device has written back a completion status */
    dma[status_off] = 0xFFu;

    /* Compiler + hardware barrier: header writes must complete before we
     * publish the descriptor to the device */
    ARCH_WMB();

    /* ── Step 2: Build the descriptor chain in queue_mem ──
     *
     * Reads and writes use a 3-descriptor chain:
     *   desc[0]: header (read-only to device, NEXT→1)
     *   desc[1]: data   (write if read, read-only if write; NEXT→2)
     *   desc[2]: status (write-only, device fills 1 byte, no NEXT)
     *
     * Flush has no data payload and therefore uses desc[0] → desc[2]. VirtIO
     * forbids zero-length descriptors, so it must not pass through desc[1].
     * The head descriptor index placed in the available ring is always 0.
     */
    /* Descriptor 0 — request header (device reads) */
    desc[0].addr  = dma_pa + DMA_HDR_OFFSET;
    desc[0].len   = sizeof(virtio_blk_req_hdr_t);
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next  = (type == VIRTIO_BLK_T_FLUSH) ? 2u : 1u;

    /* Descriptor 1 — data buffer */
    desc[1].addr  = dma_pa + DMA_DATA_OFFSET;
    desc[1].len   = data_len;
    /* For reads the device writes into this buffer; for writes the device
     * reads from it. Flush skips this descriptor. */
    desc[1].flags = VIRTQ_DESC_F_NEXT |
                    ((type == VIRTIO_BLK_T_IN) ? VIRTQ_DESC_F_WRITE : 0u);
    desc[1].next  = 2;

    /* Descriptor 2 — status byte (device writes) */
    desc[2].addr  = dma_pa + status_off;
    desc[2].len   = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE;
    desc[2].next  = 0;

    /* ── Step 3: Publish head descriptor to available ring ── */
    /*
     * Record the used-ring index before we ring the doorbell.  After the
     * device processes the request it will advance used->idx by 1; we poll
     * for that increment.
     */
    uint16_t used_idx_before = used->idx;

    /* The available ring ring[] element and idx update must be visible to the
     * device before we write QueueNotify. */
    uint16_t avail_idx = avail->idx;
    avail->ring[avail_idx % VIRTIO_BLK_QUEUE_SIZE] = 0;
    ARCH_WMB();
    avail->idx = (uint16_t)(avail_idx + 1u);
    ARCH_WMB();

    /* ── Step 4: Kick the device ── */
    if (!aos_virtio_host_notify(&device->transport)) {
        device->error_count++;
        return BLK_ERR_IO;
    }

    /* ── Step 5: Poll for completion ──
     *
     * We spin on the used ring index.  When the device completes the request
     * it writes a virtq_used_elem_t and increments used->idx.  No interrupts
     * are used in this MVP.
     */
    uint32_t iters = 0;
    while (used->idx == used_idx_before) {
        /* Read barrier: ensure we see the device's update */
        ARCH_MB();
        if (++iters >= VIRTIO_BLK_POLL_ITERS) {
            log_drain_write(17, 17, "[virtio_blk] ERROR: I/O timeout\n");
            device->error_count++;
            return BLK_ERR_IO;
        }
    }

    /* Read barrier before inspecting the status byte the device wrote */
    ARCH_MB();

    /* ── Step 6: Check device status ── */
    uint8_t status = dma[status_off];
    if (status != VIRTIO_BLK_S_OK) {
        log_drain_write(17, 17, "[virtio_blk] ERROR: device returned non-OK status\n");
        device->error_count++;
        return BLK_ERR_IO;
    }

    return BLK_OK;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Device initialisation — called once from init()
 * ──────────────────────────────────────────────────────────────────────────── */
static void virtio_blk_device_init(blk_device_t *device, uint32_t media_id,
                                   uintptr_t mmio_vaddr, const aos_blk_pci_info_t *pci)
{
    device->initialized = false;
    device->error_count = 0;
    device->init_error = 0;
    device->media_id = media_id;
    device->queue_off = AGENTOS_BLK_MEDIA_QUEUE_OFF(media_id);
    device->dma_off = AGENTOS_BLK_MEDIA_DMA_OFF(media_id);
    aos_virtio_host_t *transport = &device->transport;

    if (mmio_vaddr == 0u && !pci) {
        device->init_error = 2u;
        return;
    }

    bool bound;
    if (pci) {
        bound = aos_virtio_host_pci(transport,
            AOS_BLK_PCI_REGION_VA(0u) + pci->offset[0], pci->length[0],
            AOS_BLK_PCI_REGION_VA(2u) + pci->offset[2], pci->length[2],
            AOS_BLK_PCI_REGION_VA(1u) + pci->offset[1], pci->length[1],
            pci->notify_multiplier);
    } else {
        /* Each ARM virtio-MMIO slot is 512 bytes, including device config. */
        bound = aos_virtio_host_mmio(transport, mmio_vaddr, 0x200u, 2u);
    }
    if (!bound) {
        device->init_error = 3u;
        return;
    }

    /* ── Initialisation sequence (virtio spec §3.1.1) ── */

    /* Step 1 — Reset the device */
    aos_virtio_host_set_status(transport, 0);
    unsigned reset_poll;
    for (reset_poll = 0; reset_poll < 100000u; reset_poll++) {
        if (!aos_virtio_host_status(transport)) break;
    }
    if (reset_poll == 100000u) {
        device->init_error = 5u;
        return;
    }

    /* Step 2 — Acknowledge: guest has seen the device */
    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE;
    aos_virtio_host_set_status(transport, status);

    /* Step 3 — Driver: guest knows how to drive this device */
    status |= VIRTIO_STATUS_DRIVER;
    aos_virtio_host_set_status(transport, status);

    /* Step 4 — Feature negotiation
     * Select word 0 of device features, read them, mask to what we want */
    uint32_t dev_features = aos_virtio_host_features(transport, 0);
    uint32_t drv_features = dev_features & VIRTIO_BLK_FEATURES_WANTED;
    device->read_only = (dev_features & VIRTIO_BLK_F_RO) != 0u;

    aos_virtio_host_set_features(transport, 0, drv_features);

    /* Feature word 1: virtio 1.x devices require VIRTIO_F_VERSION_1 (bit 32). */
    if (!(aos_virtio_host_features(transport, 1) & 1u)) {
        device->init_error = 4u;
        aos_virtio_host_set_status(transport, status | VIRTIO_STATUS_FAILED);
        return;
    }
    aos_virtio_host_set_features(transport, 1, 1u);

    /* Step 5 — Set FEATURES_OK and confirm it sticks */
    status |= VIRTIO_STATUS_FEATURES_OK;
    aos_virtio_host_set_status(transport, status);

    uint32_t confirmed = aos_virtio_host_status(transport);
    if (!(confirmed & VIRTIO_STATUS_FEATURES_OK)) {
        log_drain_write(17, 17, "[virtio_blk] ERROR: device rejected feature set\n");
        device->init_error = 6u;
        aos_virtio_host_set_status(transport, confirmed | VIRTIO_STATUS_FAILED);
        return;
    }

    /* Step 6 — Setup virtqueue 0 */

    /* Zero the queue memory so all fields start clean */
    volatile uint8_t *qm = queue_mem(device);
    for (uint32_t i = 0; i < 4096u; i++) {
        qm[i] = 0;
    }
    ARCH_WMB();

    /* Transport validates queue capacity, alignment and notification span
     * before enabling DMA. Only one descriptor chain is in flight. */
    uint64_t qpa = queue_paddr(device);
    /* Suppress used-ring interrupts: we poll instead */
    queue_avail(device)->flags = 1u;  /* VIRTQ_AVAIL_F_NO_INTERRUPT */
    ARCH_WMB();
    if (!aos_virtio_host_queue(transport, 0u, VIRTIO_BLK_QUEUE_SIZE,
                               qpa + DESC_OFFSET, qpa + AVAIL_OFFSET,
                               qpa + USED_OFFSET)) {
        log_drain_write(17, 17, "[virtio_blk] ERROR: queue setup rejected\n");
        device->init_error = 7u;
        aos_virtio_host_set_status(transport, status | VIRTIO_STATUS_FAILED);
        return;
    }

    /* Step 7 — Signal DRIVER_OK */
    status |= VIRTIO_STATUS_DRIVER_OK;
    aos_virtio_host_set_status(transport, status);

    /* ── Read device configuration ── */
    /*
     * Read capacity as two 32-bit LE words from the config space.
     * The virtio spec requires 32-bit-wide reads for config space on MMIO.
     */
    if (!aos_virtio_host_config64(transport, 0u, &device->capacity)) {
        device->init_error = 8u;
        aos_virtio_host_set_status(transport, status | VIRTIO_STATUS_FAILED);
        return;
    }

    /* Block size: at offset 20 within config space (after capacity(8) +
     * size_max(4) + seg_max(4) + geometry(4)) */
    if (drv_features & VIRTIO_BLK_F_BLK_SIZE) {
        /* blk_size is at config offset 20 (0x14) */
        if (!aos_virtio_host_config32(transport, 20u, &device->block_size)) {
            device->init_error = 8u;
            aos_virtio_host_set_status(transport, status | VIRTIO_STATUS_FAILED);
            return;
        }
        if (device->block_size == 0) {
            device->block_size = VIRTIO_BLK_DEFAULT_SECTOR_SIZE;
        }
    } else {
        device->block_size = VIRTIO_BLK_DEFAULT_SECTOR_SIZE;
    }

    device->initialized = true;
    log_drain_write(17, 17, "[virtio_blk] device initialised OK\n");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Microkit entry points
 * ──────────────────────────────────────────────────────────────────────────── */

/*
 * init() — called once by the Microkit runtime before any IPC arrives.
 * No main(); Microkit provides the entry point.
 */
static void virtio_blk_pd_init(void)
{
    const agentos_blk_shared_meta_t *shared =
        (const agentos_blk_shared_meta_t *)AGENTOS_BLK_SHARED_VA;

    agentos_log_boot("virtio_blk");
    log_drain_write(17, 17, "[virtio_blk] Initializing virtio-blk driver...\n");

    if (blk_mmio_vaddr == 0u)
        blk_mmio_vaddr = AGENTOS_HOST_BLK_MMIO_VA;
    if (blk_dma_shmem_vaddr == 0u)
        blk_dma_shmem_vaddr =
            AGENTOS_BLK_SHARED_VA + AGENTOS_BLK_SHARED_DMA_OFF;

    if (shared->magic != AGENTOS_BLK_SHARED_MAGIC ||
        (shared->version != 1u && shared->version != 2u) ||
        shared->size != AGENTOS_BLK_SHARED_SIZE) {
        log_drain_write(17, 17, "[virtio_blk] ERROR: shared DMA metadata invalid\n");
        return;
    }
    g_blk_shared_paddr = shared->paddr;

    const aos_blk_pci_info_t *pci = NULL;
    if (shared->version == 2u) {
        pci = (const aos_blk_pci_info_t *)(AGENTOS_BLK_SHARED_VA + AOS_BLK_PCI_INFO_OFF);
        if (pci->magic != AOS_BLK_PCI_INFO_MAGIC || pci->version != 1u) return;
        for (unsigned r = 0; r < 3u; r++) {
            if (pci->offset[r] >= 4096u || !pci->length[r] ||
                pci->length[r] > 4096u - pci->offset[r]) return;
        }
    }

    virtio_blk_device_init(
        &dev[AOS_HOST_BLK_MEDIA_PRIMARY],
        AOS_HOST_BLK_MEDIA_PRIMARY,
        blk_mmio_vaddr, pci);
    if (!pci) virtio_blk_device_init(
        &dev[AOS_HOST_BLK_MEDIA_SECONDARY],
        AOS_HOST_BLK_MEDIA_SECONDARY,
        AGENTOS_HOST_SECONDARY_BLK_PAGE_VA +
            AGENTOS_HOST_SECONDARY_BLK_PAGE_OFF, NULL);

    if (dev[AOS_HOST_BLK_MEDIA_PRIMARY].initialized ||
        dev[AOS_HOST_BLK_MEDIA_SECONDARY].initialized) {
        log_drain_write(17, 17, "[virtio_blk] READY\n");
    } else {
        log_drain_write(17, 17, "[virtio_blk] WARNING: device absent, all ops return BLK_ERR_NODEV\n");
    }
}

/*
 * notified() — called when a notification (non-PPC signal) arrives.
 * virtio_blk is passive (priority 175, passive="true") so it should not
 * normally receive notifications, but we handle them gracefully.
 */
static void virtio_blk_pd_notified(uint32_t ch)
{
    /* Log unexpected notification and ignore */
    agentos_log_channel("virtio_blk", ch);
}

/*
 * protected() — PPC handler; all block I/O operations arrive here.
 *
 * Channel 0 (VIRTIO_BLK_CH_CONTROLLER): admin/status calls from controller
 * Channel 1 (VIRTIO_BLK_CH_VFS):        block I/O calls from vfs_server
 *
 * Both channels carry the same opcode space (OP_BLK_*); the channel
 * argument is available for future per-caller access-control policy.
 */
static bool blk_canonical_op(uint32_t op)
{
    return op >= AOS_HOST_BLK_OP_READ && op <= AOS_HOST_BLK_OP_HEALTH;
}

static uint32_t blk_wire_status(uint32_t op, uint32_t status)
{
    if (!blk_canonical_op(op)) return status;
    switch (status) {
    case BLK_OK:        return AOS_HOST_BLK_OK;
    case BLK_ERR_NODEV: return AOS_HOST_BLK_ERR_NODEV;
    case BLK_ERR_OOB:   return AOS_HOST_BLK_ERR_OOB;
    default:            return AOS_HOST_BLK_ERR_IO;
    }
}

static uint32_t virtio_blk_h_dispatch(sel4_badge_t b, const sel4_msg_t *req,
                                      sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)ctx;
    uint32_t op = (uint32_t)msg_u32(req, 0);
    uint32_t media_id = blk_canonical_op(op) && req->length >= 20u
        ? (uint32_t)msg_u32(req, 16) : AOS_HOST_BLK_MEDIA_PRIMARY;
    blk_device_t *device;

    if (media_id >= AOS_HOST_BLK_MEDIA_COUNT) {
        rep_u32(rep, 0, blk_canonical_op(op) ? 4u : BLK_ERR_NODEV);
        rep->length = 4u;
        return SEL4_ERR_OK;
    }
    device = &dev[media_id];

    switch (op) {

    /* ── OP_BLK_READ ────────────────────────────────────────────────────── */
    case OP_BLK_READ: {
        if (!device->initialized) {
            rep_u32(rep, 0, blk_wire_status(op, BLK_ERR_NODEV));
            rep->length = 4;
            return SEL4_ERR_OK;
        }

        uint32_t block_lo = (uint32_t)msg_u32(req, 4);
        uint32_t block_hi = (uint32_t)msg_u32(req, 8);
        uint32_t count    = (uint32_t)msg_u32(req, 12);
        uint64_t sector   = ((uint64_t)block_hi << 32) | (uint64_t)block_lo;

        if (count == 0) {
            rep_u32(rep, 0, blk_wire_status(op, BLK_OK));
            rep->length = 4;
            return SEL4_ERR_OK;
        }
        if (count > dma_max_sectors(device)) {
            rep_u32(rep, 0, blk_wire_status(op, BLK_ERR_IO));
            rep->length = 4;
            return SEL4_ERR_OK;
        }
        if (device->capacity > 0 &&
            (sector > device->capacity ||
             (uint64_t)count > device->capacity - sector)) {
            rep_u32(rep, 0, blk_wire_status(op, BLK_ERR_OOB));
            rep->length = 4;
            return SEL4_ERR_OK;
        }

        uint32_t rc = virtio_blk_do_io(device, VIRTIO_BLK_T_IN,
                                       sector, count);
        rep_u32(rep, 0, blk_wire_status(op, rc));
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    /* ── OP_BLK_WRITE ───────────────────────────────────────────────────── */
    case OP_BLK_WRITE: {
        if (!device->initialized) {
            rep_u32(rep, 0, blk_wire_status(op, BLK_ERR_NODEV));
            rep->length = 4;
            return SEL4_ERR_OK;
        }
        if (device->read_only) {
            rep_u32(rep, 0, blk_wire_status(op, BLK_ERR_IO));
            rep->length = 4;
            return SEL4_ERR_OK;
        }

        uint32_t block_lo = (uint32_t)msg_u32(req, 4);
        uint32_t block_hi = (uint32_t)msg_u32(req, 8);
        uint32_t count    = (uint32_t)msg_u32(req, 12);
        uint64_t sector   = ((uint64_t)block_hi << 32) | (uint64_t)block_lo;

        if (count == 0) {
            rep_u32(rep, 0, blk_wire_status(op, BLK_OK));
            rep->length = 4;
            return SEL4_ERR_OK;
        }
        if (count > dma_max_sectors(device)) {
            rep_u32(rep, 0, blk_wire_status(op, BLK_ERR_IO));
            rep->length = 4;
            return SEL4_ERR_OK;
        }
        if (device->capacity > 0 &&
            (sector > device->capacity ||
             (uint64_t)count > device->capacity - sector)) {
            rep_u32(rep, 0, blk_wire_status(op, BLK_ERR_OOB));
            rep->length = 4;
            return SEL4_ERR_OK;
        }

        uint32_t rc = virtio_blk_do_io(device, VIRTIO_BLK_T_OUT,
                                       sector, count);
        rep_u32(rep, 0, blk_wire_status(op, rc));
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    /* ── OP_BLK_FLUSH ───────────────────────────────────────────────────── */
    case OP_BLK_FLUSH: {
        if (!device->initialized) {
            rep_u32(rep, 0, blk_wire_status(op, BLK_ERR_NODEV));
            rep->length = 4;
            return SEL4_ERR_OK;
        }

        /* Send a flush request (sector and count are ignored by the device) */
        uint32_t rc = virtio_blk_do_io(device, VIRTIO_BLK_T_FLUSH, 0, 0);
        rep_u32(rep, 0, blk_wire_status(op, rc));
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    /* ── OP_BLK_INFO ────────────────────────────────────────────────────── */
    case OP_BLK_INFO: {
        if (!device->initialized) {
            rep_u32(rep, 0, blk_wire_status(op, BLK_ERR_NODEV));
            rep->length = 4;
            return SEL4_ERR_OK;
        }

        rep_u32(rep, 0, blk_wire_status(op, BLK_OK));
        rep_u32(rep, 4, (uint32_t)(device->capacity & 0xFFFFFFFFu));
        rep_u32(rep, 8, (uint32_t)(device->capacity >> 32));
        rep_u32(rep, 12, device->block_size);
        rep_u32(rep, 16, device->read_only ? AOS_HOST_BLK_INFO_READ_ONLY : 0u);
        rep->length = 20;
        return SEL4_ERR_OK;
    }

    /* ── OP_BLK_HEALTH ──────────────────────────────────────────────────── */
    case OP_BLK_HEALTH: {
        rep_u32(rep, 0, blk_wire_status(op, BLK_OK));
        rep_u32(rep, 4, device->initialized ? 1u : 0u);
        rep_u32(rep, 8, device->error_count);
        rep_u32(rep, 12, device->init_error);
        rep->length = 16;
        return SEL4_ERR_OK;
    }

    /* ── Unknown opcode ─────────────────────────────────────────────────── */
    default: {
        log_drain_write(17, 17, "[virtio_blk] WARNING: unknown opcode received\n");
        rep_u32(rep, 0, BLK_ERR_IO);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    } /* switch (op) */
}

/* ── E5-S8: Entry point ─────────────────────────────────────────────────── */
void virtio_blk_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    virtio_blk_pd_init();
    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    /* Dispatch all opcodes through the generic handler */
    sel4_server_register(&srv, SEL4_SERVER_OPCODE_ANY, virtio_blk_h_dispatch, (void *)0);
    sel4_server_run(&srv);
}

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep) { virtio_blk_main(my_ep, ns_ep); }
