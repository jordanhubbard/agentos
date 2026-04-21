/*
 * block_pd.c — OS-neutral block device service PD
 *
 * Abstracts virtio_blk behind a PD boundary.  Callers see a partition-level
 * handle-based block API (MSG_BLOCK_*); the virtio transport is an internal
 * implementation detail.
 *
 * Channels (from block_pd's perspective):
 *   id=0: passive inbound — callers PPC in with MSG_BLOCK_* requests
 *   id=1: active outbound — block_pd PPCs into virtio_blk (OP_BLK_*)
 *
 * Shared memory:
 *   block_shmem (64KB): data buffer exchanged with callers
 *     Callers write sector data here before MSG_BLOCK_WRITE.
 *     block_pd writes sector data here after MSG_BLOCK_READ.
 *   blk_dma_shmem (32KB): DMA buffer shared with virtio_blk
 *     block_pd copies data to/from this region when forwarding I/O.
 *
 * Per-guest isolation:
 *   Handles are bound to the microkit_channel that opened them.
 *   A guest PD cannot read, write, flush, or close another guest's handle.
 *
 * Partition support (MVP):
 *   partition=0 maps the entire device (lba_base=0).
 *   Other partition indices return BLOCK_ERR_BAD_PART in this release.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "virtio_blk.h"
#include "contracts/block_contract.h"
#include "arch_barrier.h"

/* ── Channel assignments (from block_pd's perspective) ──────────────────── */
#define BPD_CH_IN    0u   /* callers PPC in */
#define BPD_CH_VBLK  1u   /* block_pd PPCs into virtio_blk */

/* ── Setvar symbols written by Microkit before init() ───────────────────── */

/* 64KB data shmem shared with callers (write data here for WRITE; read after READ) */
uintptr_t block_shmem_vaddr;

/* 32KB DMA shmem shared with virtio_blk (fixed_mr: vaddr == paddr) */
uintptr_t blk_dma_shmem_vaddr;

/* Declared for log_drain_write(); left unmapped so log calls fall back to
 * microkit_dbg_puts (no log_drain channel required). */
uintptr_t log_drain_rings_vaddr;

/* ── DMA layout mirrors virtio_blk.c ────────────────────────────────────── */
#define BPD_DMA_HDR_OFFSET   0u
#define BPD_DMA_DATA_OFFSET  16u   /* request header is 16 bytes */
#define BPD_DMA_MAX_SECTORS  63u   /* floor((32768 - 16 - 1) / 512) */
#define BPD_SECTOR_SIZE      512u

/* ── Handle table ────────────────────────────────────────────────────────── */

typedef struct {
    bool             active;
    microkit_channel owner;        /* channel that issued MSG_BLOCK_OPEN */
    uint32_t         dev_id;
    uint32_t         partition;
    uint32_t         flags;        /* BLOCK_OPEN_FLAG_* */
    uint64_t         lba_base;     /* partition start (sectors) */
    uint64_t         sector_count; /* partition size (sectors) */
    uint32_t         block_size;   /* bytes per sector reported by device */
} bpd_handle_t;

static bpd_handle_t handles[BLOCK_MAX_CLIENTS];

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static int alloc_handle(void)
{
    for (uint32_t i = 0; i < BLOCK_MAX_CLIENTS; i++) {
        if (!handles[i].active)
            return i;
    }
    return -1;
}

/* Validate handle index and ownership.  Returns true if ok. */
static bool validate_handle(uint32_t h, microkit_channel ch)
{
    if (h == 0 || h > (uint32_t)BLOCK_MAX_CLIENTS)
        return false;
    int idx = (int)h - 1;
    if (!handles[idx].active)
        return false;
    if (handles[idx].owner != ch)
        return false;
    return true;
}

/* Call OP_BLK_INFO on virtio_blk.  Returns true on success. */
static bool vblk_info(uint64_t *out_capacity, uint32_t *out_block_size)
{
    microkit_msginfo reply =
        microkit_ppcall(BPD_CH_VBLK, microkit_msginfo_new(OP_BLK_INFO, 0));
    uint32_t rc = (uint32_t)microkit_mr_get(0);
    if (rc != BLK_OK)
        return false;
    uint32_t cap_lo    = (uint32_t)microkit_mr_get(1);
    uint32_t cap_hi    = (uint32_t)microkit_mr_get(2);
    *out_capacity      = ((uint64_t)cap_hi << 32) | (uint64_t)cap_lo;
    *out_block_size    = (uint32_t)microkit_mr_get(3);
    (void)reply;
    return true;
}

/* Copy n bytes from src to dst using 32-bit words where possible. */
static void bpd_memcpy(volatile uint8_t *dst, volatile uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        dst[i] = src[i];
}

/* ── MSG_BLOCK_OPEN ──────────────────────────────────────────────────────── */
/*
 * MR1=dev_id, MR2=partition, MR3=flags
 * Reply: MR0=ok (0=BLOCK_OK), MR1=handle
 */
static microkit_msginfo handle_open(microkit_channel ch, microkit_msginfo msg)
{
    (void)msg;
    uint32_t dev_id    = (uint32_t)microkit_mr_get(1);
    uint32_t partition = (uint32_t)microkit_mr_get(2);
    uint32_t flags     = (uint32_t)microkit_mr_get(3);

    /* Only device 0 is supported in this release */
    if (dev_id != 0) {
        microkit_mr_set(0, BLOCK_ERR_BAD_DEV);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    /* Only whole-device partition (0) in this MVP */
    if (partition != 0) {
        microkit_mr_set(0, BLOCK_ERR_BAD_PART);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    /* Query device geometry */
    uint64_t capacity   = 0;
    uint32_t block_size = BPD_SECTOR_SIZE;
    if (!vblk_info(&capacity, &block_size)) {
        microkit_mr_set(0, BLOCK_ERR_BAD_DEV);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    int idx = alloc_handle();
    if (idx < 0) {
        microkit_mr_set(0, BLOCK_ERR_NO_SLOTS);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    handles[idx].active       = true;
    handles[idx].owner        = ch;
    handles[idx].dev_id       = dev_id;
    handles[idx].partition    = partition;
    handles[idx].flags        = flags;
    handles[idx].lba_base     = 0;
    handles[idx].sector_count = capacity;
    handles[idx].block_size   = block_size;

    microkit_mr_set(0, BLOCK_OK);
    microkit_mr_set(1, (uint32_t)(idx + 1));  /* handle = idx + 1 */
    return microkit_msginfo_new(0, 2);
}

/* ── MSG_BLOCK_CLOSE ─────────────────────────────────────────────────────── */
/*
 * MR1=handle
 * Reply: MR0=ok
 */
static microkit_msginfo handle_close(microkit_channel ch, microkit_msginfo msg)
{
    (void)msg;
    uint32_t h = (uint32_t)microkit_mr_get(1);

    if (!validate_handle(h, ch)) {
        microkit_mr_set(0, BLOCK_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    int idx = (int)h - 1;
    handles[idx].active = false;

    microkit_mr_set(0, BLOCK_OK);
    return microkit_msginfo_new(0, 1);
}

/* ── MSG_BLOCK_READ ──────────────────────────────────────────────────────── */
/*
 * MR1=handle, MR2=lba_lo, MR3=sectors
 * Data written to block_shmem on success.
 * Reply: MR0=ok, MR1=sectors_read
 */
static microkit_msginfo handle_read(microkit_channel ch, microkit_msginfo msg)
{
    (void)msg;
    uint32_t h       = (uint32_t)microkit_mr_get(1);
    uint32_t lba_lo  = (uint32_t)microkit_mr_get(2);
    uint32_t sectors = (uint32_t)microkit_mr_get(3);

    if (!validate_handle(h, ch)) {
        microkit_mr_set(0, BLOCK_ERR_BAD_HANDLE);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    int idx = (int)h - 1;

    if (sectors == 0) {
        microkit_mr_set(0, BLOCK_OK);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    if (sectors > BLOCK_MAX_SECTORS) {
        microkit_mr_set(0, BLOCK_ERR_IO);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    uint64_t abs_lba      = handles[idx].lba_base + (uint64_t)lba_lo;
    uint64_t sector_count = handles[idx].sector_count;

    if (sector_count > 0 && (abs_lba + sectors) > sector_count) {
        microkit_mr_set(0, BLOCK_ERR_OOB);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    /* Issue in chunks of BPD_DMA_MAX_SECTORS if the request is large */
    volatile uint8_t *shmem = (volatile uint8_t *)block_shmem_vaddr;
    volatile uint8_t *dma   = (volatile uint8_t *)blk_dma_shmem_vaddr;
    uint32_t done = 0;

    while (done < sectors) {
        uint32_t batch = sectors - done;
        if (batch > BPD_DMA_MAX_SECTORS)
            batch = BPD_DMA_MAX_SECTORS;

        uint64_t cur_lba   = abs_lba + done;
        uint32_t lba_lo32  = (uint32_t)(cur_lba & 0xFFFFFFFFu);
        uint32_t lba_hi32  = (uint32_t)(cur_lba >> 32);

        microkit_mr_set(1, lba_lo32);
        microkit_mr_set(2, lba_hi32);
        microkit_mr_set(3, batch);
        microkit_msginfo reply =
            microkit_ppcall(BPD_CH_VBLK, microkit_msginfo_new(OP_BLK_READ, 3));
        uint32_t rc = (uint32_t)microkit_mr_get(0);
        (void)reply;

        if (rc != BLK_OK) {
            if (rc == BLK_ERR_OOB) {
                microkit_mr_set(0, BLOCK_ERR_OOB);
            } else {
                microkit_mr_set(0, BLOCK_ERR_IO);
            }
            microkit_mr_set(1, done);
            return microkit_msginfo_new(0, 2);
        }

        /* Copy from DMA shmem to block_shmem */
        uint32_t byte_off  = done * BPD_SECTOR_SIZE;
        uint32_t byte_cnt  = batch * BPD_SECTOR_SIZE;
        ARCH_MB();
        bpd_memcpy(shmem + byte_off,
                   dma + BPD_DMA_DATA_OFFSET,
                   byte_cnt);
        done += batch;
    }

    microkit_mr_set(0, BLOCK_OK);
    microkit_mr_set(1, sectors);
    return microkit_msginfo_new(0, 2);
}

/* ── MSG_BLOCK_WRITE ─────────────────────────────────────────────────────── */
/*
 * MR1=handle, MR2=lba_lo, MR3=sectors
 * Data sourced from block_shmem.
 * Reply: MR0=ok, MR1=sectors_written
 */
static microkit_msginfo handle_write(microkit_channel ch, microkit_msginfo msg)
{
    (void)msg;
    uint32_t h       = (uint32_t)microkit_mr_get(1);
    uint32_t lba_lo  = (uint32_t)microkit_mr_get(2);
    uint32_t sectors = (uint32_t)microkit_mr_get(3);

    if (!validate_handle(h, ch)) {
        microkit_mr_set(0, BLOCK_ERR_BAD_HANDLE);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    int idx = (int)h - 1;

    if (handles[idx].flags & BLOCK_OPEN_FLAG_RDONLY) {
        microkit_mr_set(0, BLOCK_ERR_READONLY);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    if (sectors == 0) {
        microkit_mr_set(0, BLOCK_OK);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    if (sectors > BLOCK_MAX_SECTORS) {
        microkit_mr_set(0, BLOCK_ERR_IO);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    uint64_t abs_lba      = handles[idx].lba_base + (uint64_t)lba_lo;
    uint64_t sector_count = handles[idx].sector_count;

    if (sector_count > 0 && (abs_lba + sectors) > sector_count) {
        microkit_mr_set(0, BLOCK_ERR_OOB);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    volatile uint8_t *shmem = (volatile uint8_t *)block_shmem_vaddr;
    volatile uint8_t *dma   = (volatile uint8_t *)blk_dma_shmem_vaddr;
    uint32_t done = 0;

    while (done < sectors) {
        uint32_t batch = sectors - done;
        if (batch > BPD_DMA_MAX_SECTORS)
            batch = BPD_DMA_MAX_SECTORS;

        /* Copy from block_shmem into DMA shmem data region */
        uint32_t byte_off = done * BPD_SECTOR_SIZE;
        uint32_t byte_cnt = batch * BPD_SECTOR_SIZE;
        bpd_memcpy(dma + BPD_DMA_DATA_OFFSET,
                   shmem + byte_off,
                   byte_cnt);
        ARCH_WMB();

        uint64_t cur_lba  = abs_lba + done;
        uint32_t lba_lo32 = (uint32_t)(cur_lba & 0xFFFFFFFFu);
        uint32_t lba_hi32 = (uint32_t)(cur_lba >> 32);

        microkit_mr_set(1, lba_lo32);
        microkit_mr_set(2, lba_hi32);
        microkit_mr_set(3, batch);
        microkit_msginfo reply =
            microkit_ppcall(BPD_CH_VBLK, microkit_msginfo_new(OP_BLK_WRITE, 3));
        uint32_t rc = (uint32_t)microkit_mr_get(0);
        (void)reply;

        if (rc != BLK_OK) {
            if (rc == BLK_ERR_OOB) {
                microkit_mr_set(0, BLOCK_ERR_OOB);
            } else {
                microkit_mr_set(0, BLOCK_ERR_IO);
            }
            microkit_mr_set(1, done);
            return microkit_msginfo_new(0, 2);
        }

        done += batch;
    }

    microkit_mr_set(0, BLOCK_OK);
    microkit_mr_set(1, sectors);
    return microkit_msginfo_new(0, 2);
}

/* ── MSG_BLOCK_FLUSH ─────────────────────────────────────────────────────── */
/*
 * MR1=handle
 * Reply: MR0=ok
 */
static microkit_msginfo handle_flush(microkit_channel ch, microkit_msginfo msg)
{
    (void)msg;
    uint32_t h = (uint32_t)microkit_mr_get(1);

    if (!validate_handle(h, ch)) {
        microkit_mr_set(0, BLOCK_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    microkit_msginfo reply =
        microkit_ppcall(BPD_CH_VBLK, microkit_msginfo_new(OP_BLK_FLUSH, 0));
    uint32_t rc = (uint32_t)microkit_mr_get(0);
    (void)reply;

    microkit_mr_set(0, (rc == BLK_OK) ? BLOCK_OK : BLOCK_ERR_IO);
    return microkit_msginfo_new(0, 1);
}

/* ── MSG_BLOCK_STATUS ────────────────────────────────────────────────────── */
/*
 * MR1=handle
 * Reply: MR0=ok, MR1=sector_count_lo, MR2=sector_count_hi,
 *        MR3=sector_size, MR4=flags
 */
static microkit_msginfo handle_status(microkit_channel ch, microkit_msginfo msg)
{
    (void)msg;
    uint32_t h = (uint32_t)microkit_mr_get(1);

    if (!validate_handle(h, ch)) {
        microkit_mr_set(0, BLOCK_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    int idx = (int)h - 1;

    uint32_t status_flags = 0;
    if (handles[idx].flags & BLOCK_OPEN_FLAG_RDONLY)
        status_flags |= BLOCK_STATUS_FLAG_READONLY;

    microkit_mr_set(0, BLOCK_OK);
    microkit_mr_set(1, (uint32_t)(handles[idx].sector_count & 0xFFFFFFFFu));
    microkit_mr_set(2, (uint32_t)(handles[idx].sector_count >> 32));
    microkit_mr_set(3, handles[idx].block_size);
    microkit_mr_set(4, status_flags);
    return microkit_msginfo_new(0, 5);
}

/* ── MSG_BLOCK_TRIM ──────────────────────────────────────────────────────── */
/*
 * Advisory; accepted without error.
 * MR1=handle, MR2=lba_lo, MR3=sectors
 * Reply: MR0=ok
 */
static microkit_msginfo handle_trim(microkit_channel ch, microkit_msginfo msg)
{
    (void)msg;
    uint32_t h = (uint32_t)microkit_mr_get(1);

    if (!validate_handle(h, ch)) {
        microkit_mr_set(0, BLOCK_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    microkit_mr_set(0, BLOCK_OK);
    return microkit_msginfo_new(0, 1);
}

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void)
{
    agentos_log_boot("block_pd");

    for (uint32_t i = 0; i < BLOCK_MAX_CLIENTS; i++)
        handles[i].active = false;

    log_drain_write(18, 18, "[block_pd] Ready — passive prio 165, "
                            "8 handle slots, virtio transport on ch1\n");
}

void notified(microkit_channel ch)
{
    agentos_log_channel("block_pd", ch);
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    uint64_t op = microkit_msginfo_get_label(msginfo);

    switch (op) {
    case MSG_BLOCK_OPEN:
        return handle_open(ch, msginfo);
    case MSG_BLOCK_CLOSE:
        return handle_close(ch, msginfo);
    case MSG_BLOCK_READ:
        return handle_read(ch, msginfo);
    case MSG_BLOCK_WRITE:
        return handle_write(ch, msginfo);
    case MSG_BLOCK_FLUSH:
        return handle_flush(ch, msginfo);
    case MSG_BLOCK_STATUS:
        return handle_status(ch, msginfo);
    case MSG_BLOCK_TRIM:
        return handle_trim(ch, msginfo);
    default:
        log_drain_write(18, 18, "[block_pd] WARNING: unknown opcode\n");
        microkit_mr_set(0, BLOCK_ERR_IO);
        return microkit_msginfo_new(0xFFFF, 1);
    }
}
