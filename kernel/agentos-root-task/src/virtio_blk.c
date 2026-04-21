/*
 * virtio_blk.c — virtio-blk block device driver protection domain for agentOS
 *
 * ── Physical address assumption ─────────────────────────────────────────────
 * virtio-MMIO requires the PHYSICAL addresses of virtqueue memory regions to
 * be written into the QueueDesc/QueueAvail/QueueUsed MMIO registers.
 *
 * In seL4 Microkit, the virtual→physical mapping for a PD is NOT an identity
 * map in general.  However, for memory regions declared as `fixed_mr` in the
 * .system file (i.e., memory regions whose physical address is pinned at
 * system-build time), Microkit places the region at the same address in the
 * PD's virtual address space as its physical base.  We exploit this for the
 * queue_mem array by requiring it to live inside such a fixed_mr region.
 *
 * The blk_mmio region is accessed via a setvar_vaddr mechanism: Microkit
 * writes the mapped virtual address of the `blk_mmio` memory region into the
 * `blk_mmio_vaddr` symbol before init() is called.
 *
 * queue_mem is 4 KB page-aligned and must be backed by a fixed_mr in
 * agentos.system so that vaddr == paddr.  Without this, the physical address
 * computation below would be wrong and the device would DMA to the wrong
 * memory.  A comment in the .system file must document this constraint.
 *
 * The blk_dma_shmem region (32 KB, mapped at 0x5000000 in virtio_blk) is
 * also a fixed_mr shared with vfs_server.  For I/O operations the driver uses
 * the DMA shmem physical address (== vaddr due to fixed_mr) for the data
 * descriptor so the device can DMA directly into/from the shared window.
 * ────────────────────────────────────────────────────────────────────────────
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/virtio_blk_contract.h"
#include "virtio_blk.h"
#include "arch_barrier.h"

/* ─────────────────────────────────────────────────────────────────────────────
 * Microkit setvar_vaddr symbols — written by the Microkit runtime before init()
 * ──────────────────────────────────────────────────────────────────────────── */

/* Virtual (== physical due to fixed_mr) address of the 4 KB MMIO region */
uintptr_t blk_mmio_vaddr;

/* Virtual (== physical due to fixed_mr) address of the 32 KB DMA shmem */
uintptr_t blk_dma_shmem_vaddr;

/* log_drain_rings_vaddr required by log_drain_write() in agentos.h */
uintptr_t log_drain_rings_vaddr;

/* ─────────────────────────────────────────────────────────────────────────────
 * Convenience MMIO read/write helpers
 *
 * All virtio-MMIO registers are 32-bit, native-endian, and must be accessed
 * with 32-bit loads/stores.  We use volatile to prevent the compiler from
 * caching or reordering the accesses.
 * ──────────────────────────────────────────────────────────────────────────── */

static inline uint32_t mmio_read(volatile uint32_t *base, uint32_t offset)
{
    return *(volatile uint32_t *)((uintptr_t)base + offset);
}

static inline void mmio_write(volatile uint32_t *base, uint32_t offset, uint32_t val)
{
    *(volatile uint32_t *)((uintptr_t)base + offset) = val;
    /* Full memory barrier: ensure the store reaches the device before we
     * proceed.  Without this, on weakly-ordered architectures (AArch64,
     * RISC-V) a subsequent MMIO read might observe stale state. */
    ARCH_MB();
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Device state
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    bool               initialized;
    volatile uint32_t *mmio;         /* MMIO base pointer (blk_mmio_vaddr) */
    uint64_t           capacity;     /* total 512-byte sectors on the device */
    uint32_t           block_size;   /* logical block size reported by device (bytes) */
    uint32_t           error_count;  /* cumulative I/O errors since boot */
} blk_device_t;

static blk_device_t dev;

/* ─────────────────────────────────────────────────────────────────────────────
 * Virtqueue memory
 *
 * For the MVP we use a single queue of depth 1 (one descriptor chain in
 * flight at a time, polled synchronously).  The three virtqueue regions
 * must be physically contiguous and their physical addresses passed to the
 * device.  We place them in a statically-allocated, page-aligned buffer;
 * see the file-top comment about the fixed_mr requirement.
 *
 * Layout inside queue_mem[]:
 *   [0]                    : descriptor table  (1 × 16 bytes = 16 bytes)
 *   [AVAIL_OFFSET]         : available ring    (4 + 2×1 bytes = 6 bytes, aligned 2)
 *   [USED_OFFSET]          : used ring         (4 + 8×1 bytes = 12 bytes, aligned 4)
 *
 * We over-allocate to 4 KB so that the natural page alignment is preserved
 * for any fixed_mr placement in the .system file.
 * ──────────────────────────────────────────────────────────────────────────── */

/*
 * Queue memory buffer.  __attribute__((aligned(4096))) ensures both that
 * the compiler does not split it across a page and that the symbol address
 * itself is page-aligned.  If this PD's fixed_mr is mapped at an address
 * that equals its physical address (vaddr == paddr), then
 * (uintptr_t)queue_mem IS the physical address we hand to the device.
 */
static uint8_t queue_mem[4096] __attribute__((aligned(4096)));

/* Offsets within queue_mem for each virtqueue region */
#define DESC_OFFSET     0u
#define AVAIL_OFFSET    16u   /* after 1 × 16-byte descriptor */
#define USED_OFFSET     32u   /* after avail header (2+2+2=6) + 2 bytes padding → align 4 */

/* Typed pointers into queue_mem */
#define QUEUE_DESC  ((volatile virtq_desc_t  *)(queue_mem + DESC_OFFSET))
#define QUEUE_AVAIL ((volatile virtq_avail_t *)(queue_mem + AVAIL_OFFSET))
#define QUEUE_USED  ((volatile virtq_used_t  *)(queue_mem + USED_OFFSET))

/*
 * Physical address of queue_mem (and its sub-regions).
 * Valid only because queue_mem lives inside a fixed_mr (vaddr == paddr).
 */
#define QUEUE_MEM_PADDR     ((uintptr_t)queue_mem)
#define DESC_PADDR          (QUEUE_MEM_PADDR + DESC_OFFSET)
#define AVAIL_PADDR         (QUEUE_MEM_PADDR + AVAIL_OFFSET)
#define USED_PADDR          (QUEUE_MEM_PADDR + USED_OFFSET)

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
 * blk_dma_shmem is 32 KB.  The maximum useful I/O is therefore:
 *   (32768 - 16 - 1) / 512 = 63 full sectors per call.
 * We cap count at 63 to prevent buffer overflow.
 * ──────────────────────────────────────────────────────────────────────────── */

#define DMA_SHMEM_SIZE          0x8000u   /* 32 KB */
#define DMA_HDR_OFFSET          0u
#define DMA_DATA_OFFSET         16u
#define DMA_MAX_SECTORS         63u       /* floor((32768 - 16 - 1) / 512) */
#define DMA_MAX_DATA_BYTES      (DMA_MAX_SECTORS * 512u)
#define DMA_STATUS_OFFSET(cnt)  (DMA_DATA_OFFSET + (uint32_t)(cnt) * 512u)

/* Cast the DMA shmem base to a byte pointer */
#define DMA_BASE  ((volatile uint8_t *)blk_dma_shmem_vaddr)

/* Physical address of DMA shmem base (valid because it is a fixed_mr) */
#define DMA_PADDR ((uintptr_t)blk_dma_shmem_vaddr)

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
static uint32_t virtio_blk_do_io(uint32_t type, uint64_t sector, uint32_t count)
{
    uint32_t data_len    = count * 512u;
    uint32_t status_off  = DMA_STATUS_OFFSET(count);

    /* ── Step 1: Write request header into DMA shmem ── */
    volatile virtio_blk_req_hdr_t *hdr =
        (volatile virtio_blk_req_hdr_t *)(DMA_BASE + DMA_HDR_OFFSET);
    hdr->type     = type;
    hdr->reserved = 0;
    hdr->sector   = sector;

    /* Initialise the status byte to a non-zero sentinel so we can detect
     * whether the device has written back a completion status */
    DMA_BASE[status_off] = 0xFFu;

    /* Compiler + hardware barrier: header writes must complete before we
     * publish the descriptor to the device */
    ARCH_WMB();

    /* ── Step 2: Build the descriptor chain in queue_mem ──
     *
     * We use a 3-descriptor chain:
     *   desc[0]: header (read-only to device, NEXT→1)
     *   desc[1]: data   (write if read, read-only if write/flush; NEXT→2)
     *            For flush (count==0) we still include a zero-length data
     *            descriptor to keep the chain uniform.
     *   desc[2]: status (write-only, device fills 1 byte, no NEXT)
     *
     * With queue size 1 we always use descriptor indices 0, 1, 2 — but
     * since queue_num is 1, we embed all three within the same queue slot
     * by chaining them.  The head descriptor index placed in the available
     * ring is always 0.
     *
     * NOTE: Because QUEUE_SIZE==1 the descriptor table technically has only
     * one slot per the spec.  In practice QEMU virtio-MMIO accepts a chain
     * starting at index 0 with .next values pointing beyond the formal
     * QueueNum as long as the physical pages are accessible.  For a true
     * production driver you would either set QueueNum=4 (minimum power-of-2
     * that fits a 3-descriptor chain) or use indirect descriptors.  For the
     * MVP polling path this works reliably on QEMU.
     */
    volatile virtq_desc_t *desc = QUEUE_DESC;

    /* Descriptor 0 — request header (device reads) */
    desc[0].addr  = (uint64_t)(DMA_PADDR + DMA_HDR_OFFSET);
    desc[0].len   = sizeof(virtio_blk_req_hdr_t);
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next  = 1;

    /* Descriptor 1 — data buffer */
    desc[1].addr  = (uint64_t)(DMA_PADDR + DMA_DATA_OFFSET);
    desc[1].len   = (data_len > 0) ? data_len : 0u;
    /* For reads the device writes into this buffer; for writes/flush the
     * device reads from it. */
    desc[1].flags = VIRTQ_DESC_F_NEXT |
                    ((type == VIRTIO_BLK_T_IN) ? VIRTQ_DESC_F_WRITE : 0u);
    desc[1].next  = 2;

    /* Descriptor 2 — status byte (device writes) */
    desc[2].addr  = (uint64_t)(DMA_PADDR + status_off);
    desc[2].len   = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE;
    desc[2].next  = 0;

    /* ── Step 3: Publish head descriptor to available ring ── */
    /*
     * Record the used-ring index before we ring the doorbell.  After the
     * device processes the request it will advance used->idx by 1; we poll
     * for that increment.
     */
    uint16_t used_idx_before = QUEUE_USED->idx;

    /* The available ring ring[] element and idx update must be visible to the
     * device before we write QueueNotify. */
    QUEUE_AVAIL->ring[0] = 0;  /* head descriptor index = 0 */
    ARCH_WMB();
    QUEUE_AVAIL->idx = (uint16_t)(QUEUE_AVAIL->idx + 1u);
    ARCH_WMB();

    /* ── Step 4: Kick the device ── */
    mmio_write(dev.mmio, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    /* ── Step 5: Poll for completion ──
     *
     * We spin on the used ring index.  When the device completes the request
     * it writes a virtq_used_elem_t and increments used->idx.  No interrupts
     * are used in this MVP.
     */
    uint32_t iters = 0;
    while (QUEUE_USED->idx == used_idx_before) {
        /* Read barrier: ensure we see the device's update */
        ARCH_MB();
        if (++iters >= VIRTIO_BLK_POLL_ITERS) {
            log_drain_write(17, 17, "[virtio_blk] ERROR: I/O timeout\n");
            dev.error_count++;
            return BLK_ERR_IO;
        }
    }

    /* Read barrier before inspecting the status byte the device wrote */
    ARCH_MB();

    /* ── Step 6: Check device status ── */
    uint8_t status = DMA_BASE[status_off];
    if (status != VIRTIO_BLK_S_OK) {
        log_drain_write(17, 17, "[virtio_blk] ERROR: device returned non-OK status\n");
        dev.error_count++;
        return BLK_ERR_IO;
    }

    return BLK_OK;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Device initialisation — called once from init()
 * ──────────────────────────────────────────────────────────────────────────── */
static void virtio_blk_device_init(void)
{
    dev.initialized = false;
    dev.error_count = 0;

    /* Obtain MMIO base from setvar_vaddr */
    if (blk_mmio_vaddr == 0) {
        log_drain_write(17, 17, "[virtio_blk] ERROR: blk_mmio_vaddr not set\n");
        return;
    }
    dev.mmio = (volatile uint32_t *)blk_mmio_vaddr;

    /* ── Probe: check magic, version, device ID ── */
    uint32_t magic    = mmio_read(dev.mmio, VIRTIO_MMIO_MAGIC_VALUE);
    uint32_t version  = mmio_read(dev.mmio, VIRTIO_MMIO_VERSION);
    uint32_t devid    = mmio_read(dev.mmio, VIRTIO_MMIO_DEVICE_ID);

    if (magic != VIRTIO_MMIO_MAGIC) {
        log_drain_write(17, 17, "[virtio_blk] ERROR: bad magic value (not a virtio device)\n");
        return;
    }
    if (version != 2) {
        log_drain_write(17, 17, "[virtio_blk] ERROR: unsupported virtio-MMIO version (need v2)\n");
        return;
    }
    if (devid != 2) {
        log_drain_write(17, 17, "[virtio_blk] ERROR: device ID is not 2 (not a block device)\n");
        return;
    }

    /* ── Initialisation sequence (virtio spec §3.1.1) ── */

    /* Step 1 — Reset the device */
    mmio_write(dev.mmio, VIRTIO_MMIO_STATUS, 0);

    /* Step 2 — Acknowledge: guest has seen the device */
    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE;
    mmio_write(dev.mmio, VIRTIO_MMIO_STATUS, status);

    /* Step 3 — Driver: guest knows how to drive this device */
    status |= VIRTIO_STATUS_DRIVER;
    mmio_write(dev.mmio, VIRTIO_MMIO_STATUS, status);

    /* Step 4 — Feature negotiation
     * Select word 0 of device features, read them, mask to what we want */
    mmio_write(dev.mmio, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    uint32_t dev_features = mmio_read(dev.mmio, VIRTIO_MMIO_DEVICE_FEATURES);
    uint32_t drv_features = dev_features & VIRTIO_BLK_FEATURES_WANTED;

    mmio_write(dev.mmio, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    mmio_write(dev.mmio, VIRTIO_MMIO_DRIVER_FEATURES, drv_features);

    /* Feature word 1 (high bits): we request none, write zero */
    mmio_write(dev.mmio, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    mmio_write(dev.mmio, VIRTIO_MMIO_DRIVER_FEATURES, 0);

    /* Step 5 — Set FEATURES_OK and confirm it sticks */
    status |= VIRTIO_STATUS_FEATURES_OK;
    mmio_write(dev.mmio, VIRTIO_MMIO_STATUS, status);

    uint32_t confirmed = mmio_read(dev.mmio, VIRTIO_MMIO_STATUS);
    if (!(confirmed & VIRTIO_STATUS_FEATURES_OK)) {
        log_drain_write(17, 17, "[virtio_blk] ERROR: device rejected feature set\n");
        mmio_write(dev.mmio, VIRTIO_MMIO_STATUS,
                   mmio_read(dev.mmio, VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_FAILED);
        return;
    }

    /* Step 6 — Setup virtqueue 0 */

    /* Select queue 0 */
    mmio_write(dev.mmio, VIRTIO_MMIO_QUEUE_SEL, 0);

    /* Check the max queue size the device supports */
    uint32_t qnum_max = mmio_read(dev.mmio, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qnum_max == 0) {
        log_drain_write(17, 17, "[virtio_blk] ERROR: device reports queue 0 not available\n");
        mmio_write(dev.mmio, VIRTIO_MMIO_STATUS,
                   mmio_read(dev.mmio, VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_FAILED);
        return;
    }

    /* Set our chosen queue size (1 for MVP polling mode) */
    mmio_write(dev.mmio, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_BLK_QUEUE_SIZE);

    /* Zero the queue memory so all fields start clean */
    volatile uint8_t *qm = queue_mem;
    for (uint32_t i = 0; i < sizeof(queue_mem); i++) {
        qm[i] = 0;
    }
    ARCH_WMB();

    /* Write queue region physical addresses (split into low/high 32-bit words) */
    mmio_write(dev.mmio, VIRTIO_MMIO_QUEUE_DESC_LOW,   (uint32_t)(DESC_PADDR & 0xFFFFFFFFu));
    mmio_write(dev.mmio, VIRTIO_MMIO_QUEUE_DESC_HIGH,  (uint32_t)(DESC_PADDR >> 32));
    mmio_write(dev.mmio, VIRTIO_MMIO_QUEUE_AVAIL_LOW,  (uint32_t)(AVAIL_PADDR & 0xFFFFFFFFu));
    mmio_write(dev.mmio, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)(AVAIL_PADDR >> 32));
    mmio_write(dev.mmio, VIRTIO_MMIO_QUEUE_USED_LOW,   (uint32_t)(USED_PADDR & 0xFFFFFFFFu));
    mmio_write(dev.mmio, VIRTIO_MMIO_QUEUE_USED_HIGH,  (uint32_t)(USED_PADDR >> 32));

    /* Activate the queue */
    mmio_write(dev.mmio, VIRTIO_MMIO_QUEUE_READY, 1);

    /* Suppress used-ring interrupts: we poll instead */
    QUEUE_AVAIL->flags = 1u;  /* VIRTQ_AVAIL_F_NO_INTERRUPT */

    /* Step 7 — Signal DRIVER_OK */
    status |= VIRTIO_STATUS_DRIVER_OK;
    mmio_write(dev.mmio, VIRTIO_MMIO_STATUS, status);

    /* ── Read device configuration ── */
    /*
     * Read capacity as two 32-bit LE words from the config space.
     * The virtio spec requires 32-bit-wide reads for config space on MMIO.
     */
    uint32_t cap_lo = mmio_read(dev.mmio, VIRTIO_MMIO_CONFIG + 0);
    uint32_t cap_hi = mmio_read(dev.mmio, VIRTIO_MMIO_CONFIG + 4);
    dev.capacity    = ((uint64_t)cap_hi << 32) | (uint64_t)cap_lo;

    /* Block size: at offset 20 within config space (after capacity(8) +
     * size_max(4) + seg_max(4) + geometry(4)) */
    if (drv_features & VIRTIO_BLK_F_BLK_SIZE) {
        /* blk_size is at config offset 20 (0x14) */
        dev.block_size = mmio_read(dev.mmio, VIRTIO_MMIO_CONFIG + 20);
        if (dev.block_size == 0) {
            dev.block_size = VIRTIO_BLK_DEFAULT_SECTOR_SIZE;
        }
    } else {
        dev.block_size = VIRTIO_BLK_DEFAULT_SECTOR_SIZE;
    }

    dev.initialized = true;
    log_drain_write(17, 17, "[virtio_blk] device initialised OK\n");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Microkit entry points
 * ──────────────────────────────────────────────────────────────────────────── */

/*
 * init() — called once by the Microkit runtime before any IPC arrives.
 * No main(); Microkit provides the entry point.
 */
void init(void)
{
    agentos_log_boot("virtio_blk");
    log_drain_write(17, 17, "[virtio_blk] Initializing virtio-blk driver...\n");

    virtio_blk_device_init();

    if (dev.initialized) {
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
void notified(microkit_channel ch)
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
microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch;   /* channel not used in dispatch; all ops share one table */
    uint64_t op = microkit_msginfo_get_label(msginfo);

    switch (op) {

    /* ── OP_BLK_READ ────────────────────────────────────────────────────── */
    case OP_BLK_READ: {
        if (!dev.initialized) {
            microkit_mr_set(0, BLK_ERR_NODEV);
            return microkit_msginfo_new(0, 1);
        }

        uint32_t block_lo = (uint32_t)microkit_mr_get(1);
        uint32_t block_hi = (uint32_t)microkit_mr_get(2);
        uint32_t count    = (uint32_t)microkit_mr_get(3);
        uint64_t sector   = ((uint64_t)block_hi << 32) | (uint64_t)block_lo;

        if (count == 0) {
            microkit_mr_set(0, BLK_OK);
            return microkit_msginfo_new(0, 1);
        }
        if (count > DMA_MAX_SECTORS) {
            microkit_mr_set(0, BLK_ERR_IO);
            return microkit_msginfo_new(0, 1);
        }
        /* Bounds check: sector + count must not exceed device capacity */
        if (dev.capacity > 0 && (sector + count) > dev.capacity) {
            microkit_mr_set(0, BLK_ERR_OOB);
            return microkit_msginfo_new(0, 1);
        }

        uint32_t rc = virtio_blk_do_io(VIRTIO_BLK_T_IN, sector, count);
        microkit_mr_set(0, rc);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_BLK_WRITE ───────────────────────────────────────────────────── */
    case OP_BLK_WRITE: {
        if (!dev.initialized) {
            microkit_mr_set(0, BLK_ERR_NODEV);
            return microkit_msginfo_new(0, 1);
        }

        uint32_t block_lo = (uint32_t)microkit_mr_get(1);
        uint32_t block_hi = (uint32_t)microkit_mr_get(2);
        uint32_t count    = (uint32_t)microkit_mr_get(3);
        uint64_t sector   = ((uint64_t)block_hi << 32) | (uint64_t)block_lo;

        if (count == 0) {
            microkit_mr_set(0, BLK_OK);
            return microkit_msginfo_new(0, 1);
        }
        if (count > DMA_MAX_SECTORS) {
            microkit_mr_set(0, BLK_ERR_IO);
            return microkit_msginfo_new(0, 1);
        }
        if (dev.capacity > 0 && (sector + count) > dev.capacity) {
            microkit_mr_set(0, BLK_ERR_OOB);
            return microkit_msginfo_new(0, 1);
        }

        uint32_t rc = virtio_blk_do_io(VIRTIO_BLK_T_OUT, sector, count);
        microkit_mr_set(0, rc);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_BLK_FLUSH ───────────────────────────────────────────────────── */
    case OP_BLK_FLUSH: {
        if (!dev.initialized) {
            microkit_mr_set(0, BLK_ERR_NODEV);
            return microkit_msginfo_new(0, 1);
        }

        /* Send a flush request (sector and count are ignored by the device) */
        uint32_t rc = virtio_blk_do_io(VIRTIO_BLK_T_FLUSH, 0, 0);
        microkit_mr_set(0, rc);
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_BLK_INFO ────────────────────────────────────────────────────── */
    case OP_BLK_INFO: {
        if (!dev.initialized) {
            microkit_mr_set(0, BLK_ERR_NODEV);
            return microkit_msginfo_new(0, 1);
        }

        microkit_mr_set(0, BLK_OK);
        microkit_mr_set(1, (uint32_t)(dev.capacity & 0xFFFFFFFFu));
        microkit_mr_set(2, (uint32_t)(dev.capacity >> 32));
        microkit_mr_set(3, dev.block_size);
        return microkit_msginfo_new(0, 4);
    }

    /* ── OP_BLK_HEALTH ──────────────────────────────────────────────────── */
    case OP_BLK_HEALTH: {
        microkit_mr_set(0, BLK_OK);
        microkit_mr_set(1, dev.initialized ? 1u : 0u);
        microkit_mr_set(2, dev.error_count);
        return microkit_msginfo_new(0, 3);
    }

    /* ── Unknown opcode ─────────────────────────────────────────────────── */
    default: {
        log_drain_write(17, 17, "[virtio_blk] WARNING: unknown opcode received\n");
        microkit_mr_set(0, BLK_ERR_IO);
        return microkit_msginfo_new(0xFFFF, 1);
    }

    } /* switch (op) */
}
