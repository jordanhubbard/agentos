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
 *   Handles are bound to the uint32_t that opened them.
 *   A guest PD cannot read, write, flush, or close another guest's handle.
 *
 * Partition support (MVP):
 *   partition=0 maps the entire device (lba_base=0).
 *   Other partition indices return BLOCK_ERR_BAD_PART in this release.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
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
 * sel4_dbg_puts(no log_drain channel required). */
uintptr_t log_drain_rings_vaddr;

/* ── DMA layout mirrors virtio_blk.c ────────────────────────────────────── */
#define BPD_DMA_HDR_OFFSET   0u
#define BPD_DMA_DATA_OFFSET  16u   /* request header is 16 bytes */
#define BPD_DMA_MAX_SECTORS  63u   /* floor((32768 - 16 - 1) / 512) */
#define BPD_SECTOR_SIZE      512u

/* ── Handle table ────────────────────────────────────────────────────────── */

typedef struct {
    bool             active;
    uint32_t owner;        /* channel that issued MSG_BLOCK_OPEN */
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
static bool validate_handle(uint32_t h, uint32_t ch)
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
    uint32_t reply =
        /* E5-S8: ppcall stubbed */
    uint32_t rc = (uint32_t)msg_u32(req, 0);
    if (rc != BLK_OK)
        return false;
    uint32_t cap_lo    = (uint32_t)msg_u32(req, 4);
    uint32_t cap_hi    = (uint32_t)msg_u32(req, 8);
    *out_capacity      = ((uint64_t)cap_hi << 32) | (uint64_t)cap_lo;
    *out_block_size    = (uint32_t)msg_u32(req, 12);
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
static uint32_t handle_open(uint32_t ch, uint32_t msg)
{
    (void)msg;
    uint32_t dev_id    = (uint32_t)msg_u32(req, 4);
    uint32_t partition = (uint32_t)msg_u32(req, 8);
    uint32_t flags     = (uint32_t)msg_u32(req, 12);

    /* Only device 0 is supported in this release */
    if (dev_id != 0) {
        rep_u32(rep, 0, BLOCK_ERR_BAD_DEV);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    /* Only whole-device partition (0) in this MVP */
    if (partition != 0) {
        rep_u32(rep, 0, BLOCK_ERR_BAD_PART);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    /* Query device geometry */
    uint64_t capacity   = 0;
    uint32_t block_size = BPD_SECTOR_SIZE;
    if (!vblk_info(&capacity, &block_size)) {
        rep_u32(rep, 0, BLOCK_ERR_BAD_DEV);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    int idx = alloc_handle();
    if (idx < 0) {
        rep_u32(rep, 0, BLOCK_ERR_NO_SLOTS);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    handles[idx].active       = true;
    handles[idx].owner        = ch;
    handles[idx].dev_id       = dev_id;
    handles[idx].partition    = partition;
    handles[idx].flags        = flags;
    handles[idx].lba_base     = 0;
    handles[idx].sector_count = capacity;
    handles[idx].block_size   = block_size;

    rep_u32(rep, 0, BLOCK_OK);
    rep_u32(rep, 4, (uint32_t)(idx + 1));  /* handle = idx + 1 */
    rep->length = 8;
        return SEL4_ERR_OK;
}

/* ── MSG_BLOCK_CLOSE ─────────────────────────────────────────────────────── */
/*
 * MR1=handle
 * Reply: MR0=ok
 */
static uint32_t handle_close(uint32_t ch, uint32_t msg)
{
    (void)msg;
    uint32_t h = (uint32_t)msg_u32(req, 4);

    if (!validate_handle(h, ch)) {
        rep_u32(rep, 0, BLOCK_ERR_BAD_HANDLE);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    int idx = (int)h - 1;
    handles[idx].active = false;

    rep_u32(rep, 0, BLOCK_OK);
    rep->length = 4;
        return SEL4_ERR_OK;
}

/* ── MSG_BLOCK_READ ──────────────────────────────────────────────────────── */
/*
 * MR1=handle, MR2=lba_lo, MR3=sectors
 * Data written to block_shmem on success.
 * Reply: MR0=ok, MR1=sectors_read
 */
static uint32_t handle_read(uint32_t ch, uint32_t msg)
{
    (void)msg;
    uint32_t h       = (uint32_t)msg_u32(req, 4);
    uint32_t lba_lo  = (uint32_t)msg_u32(req, 8);
    uint32_t sectors = (uint32_t)msg_u32(req, 12);

    if (!validate_handle(h, ch)) {
        rep_u32(rep, 0, BLOCK_ERR_BAD_HANDLE);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    int idx = (int)h - 1;

    if (sectors == 0) {
        rep_u32(rep, 0, BLOCK_OK);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    if (sectors > BLOCK_MAX_SECTORS) {
        rep_u32(rep, 0, BLOCK_ERR_IO);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    uint64_t abs_lba      = handles[idx].lba_base + (uint64_t)lba_lo;
    uint64_t sector_count = handles[idx].sector_count;

    if (sector_count > 0 && (abs_lba + sectors) > sector_count) {
        rep_u32(rep, 0, BLOCK_ERR_OOB);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
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

        rep_u32(rep, 4, lba_lo32);
        rep_u32(rep, 8, lba_hi32);
        rep_u32(rep, 12, batch);
        uint32_t reply =
            /* E5-S8: ppcall stubbed */
        uint32_t rc = (uint32_t)msg_u32(req, 0);
        (void)reply;

        if (rc != BLK_OK) {
            if (rc == BLK_ERR_OOB) {
                rep_u32(rep, 0, BLOCK_ERR_OOB);
            } else {
                rep_u32(rep, 0, BLOCK_ERR_IO);
            }
            rep_u32(rep, 4, done);
            rep->length = 8;
        return SEL4_ERR_OK;
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

    rep_u32(rep, 0, BLOCK_OK);
    rep_u32(rep, 4, sectors);
    rep->length = 8;
        return SEL4_ERR_OK;
}

/* ── MSG_BLOCK_WRITE ─────────────────────────────────────────────────────── */
/*
 * MR1=handle, MR2=lba_lo, MR3=sectors
 * Data sourced from block_shmem.
 * Reply: MR0=ok, MR1=sectors_written
 */
static uint32_t handle_write(uint32_t ch, uint32_t msg)
{
    (void)msg;
    uint32_t h       = (uint32_t)msg_u32(req, 4);
    uint32_t lba_lo  = (uint32_t)msg_u32(req, 8);
    uint32_t sectors = (uint32_t)msg_u32(req, 12);

    if (!validate_handle(h, ch)) {
        rep_u32(rep, 0, BLOCK_ERR_BAD_HANDLE);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    int idx = (int)h - 1;

    if (handles[idx].flags & BLOCK_OPEN_FLAG_RDONLY) {
        rep_u32(rep, 0, BLOCK_ERR_READONLY);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    if (sectors == 0) {
        rep_u32(rep, 0, BLOCK_OK);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    if (sectors > BLOCK_MAX_SECTORS) {
        rep_u32(rep, 0, BLOCK_ERR_IO);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    uint64_t abs_lba      = handles[idx].lba_base + (uint64_t)lba_lo;
    uint64_t sector_count = handles[idx].sector_count;

    if (sector_count > 0 && (abs_lba + sectors) > sector_count) {
        rep_u32(rep, 0, BLOCK_ERR_OOB);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
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

        rep_u32(rep, 4, lba_lo32);
        rep_u32(rep, 8, lba_hi32);
        rep_u32(rep, 12, batch);
        uint32_t reply =
            /* E5-S8: ppcall stubbed */
        uint32_t rc = (uint32_t)msg_u32(req, 0);
        (void)reply;

        if (rc != BLK_OK) {
            if (rc == BLK_ERR_OOB) {
                rep_u32(rep, 0, BLOCK_ERR_OOB);
            } else {
                rep_u32(rep, 0, BLOCK_ERR_IO);
            }
            rep_u32(rep, 4, done);
            rep->length = 8;
        return SEL4_ERR_OK;
        }

        done += batch;
    }

    rep_u32(rep, 0, BLOCK_OK);
    rep_u32(rep, 4, sectors);
    rep->length = 8;
        return SEL4_ERR_OK;
}

/* ── MSG_BLOCK_FLUSH ─────────────────────────────────────────────────────── */
/*
 * MR1=handle
 * Reply: MR0=ok
 */
static uint32_t handle_flush(uint32_t ch, uint32_t msg)
{
    (void)msg;
    uint32_t h = (uint32_t)msg_u32(req, 4);

    if (!validate_handle(h, ch)) {
        rep_u32(rep, 0, BLOCK_ERR_BAD_HANDLE);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    uint32_t reply =
        /* E5-S8: ppcall stubbed */
    uint32_t rc = (uint32_t)msg_u32(req, 0);
    (void)reply;

    rep_u32(rep, 0, (rc == BLK_OK) ? BLOCK_OK : BLOCK_ERR_IO);
    rep->length = 4;
        return SEL4_ERR_OK;
}

/* ── MSG_BLOCK_STATUS ────────────────────────────────────────────────────── */
/*
 * MR1=handle
 * Reply: MR0=ok, MR1=sector_count_lo, MR2=sector_count_hi,
 *        MR3=sector_size, MR4=flags
 */
static uint32_t handle_status(uint32_t ch, uint32_t msg)
{
    (void)msg;
    uint32_t h = (uint32_t)msg_u32(req, 4);

    if (!validate_handle(h, ch)) {
        rep_u32(rep, 0, BLOCK_ERR_BAD_HANDLE);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    int idx = (int)h - 1;

    uint32_t status_flags = 0;
    if (handles[idx].flags & BLOCK_OPEN_FLAG_RDONLY)
        status_flags |= BLOCK_STATUS_FLAG_READONLY;

    rep_u32(rep, 0, BLOCK_OK);
    rep_u32(rep, 4, (uint32_t)(handles[idx].sector_count & 0xFFFFFFFFu));
    rep_u32(rep, 8, (uint32_t)(handles[idx].sector_count >> 32));
    rep_u32(rep, 12, handles[idx].block_size);
    rep_u32(rep, 16, status_flags);
    rep->length = 20;
        return SEL4_ERR_OK;
}

/* ── MSG_BLOCK_TRIM ──────────────────────────────────────────────────────── */
/*
 * Advisory; accepted without error.
 * MR1=handle, MR2=lba_lo, MR3=sectors
 * Reply: MR0=ok
 */
static uint32_t handle_trim(uint32_t ch, uint32_t msg)
{
    (void)msg;
    uint32_t h = (uint32_t)msg_u32(req, 4);

    if (!validate_handle(h, ch)) {
        rep_u32(rep, 0, BLOCK_ERR_BAD_HANDLE);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    rep_u32(rep, 0, BLOCK_OK);
    rep->length = 4;
        return SEL4_ERR_OK;
}

/* ── Microkit entry points ───────────────────────────────────────────────── */

static void block_pd_pd_init(void)
{
    agentos_log_boot("block_pd");

    for (uint32_t i = 0; i < BLOCK_MAX_CLIENTS; i++)
        handles[i].active = false;

    log_drain_write(18, 18, "[block_pd] Ready — passive prio 165, "
                            "8 handle slots, virtio transport on ch1\n");
}

static void block_pd_pd_notified(uint32_t ch)
{
    agentos_log_channel("block_pd", ch);
}

static uint32_t block_pd_h_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx)
{
    (void)ctx;
    uint32_t ch = (uint32_t)b;  /* badge encodes caller channel/identity */
    uint64_t op = msg_u32(req, 0);

    switch (op) {
    case MSG_BLOCK_OPEN:
        return handle_open(ch, 0);
    case MSG_BLOCK_CLOSE:
        return handle_close(ch, 0);
    case MSG_BLOCK_READ:
        return handle_read(ch, 0);
    case MSG_BLOCK_WRITE:
        return handle_write(ch, 0);
    case MSG_BLOCK_FLUSH:
        return handle_flush(ch, 0);
    case MSG_BLOCK_STATUS:
        return handle_status(ch, 0);
    case MSG_BLOCK_TRIM:
        return handle_trim(ch, 0);
    default:
        log_drain_write(18, 18, "[block_pd] WARNING: unknown opcode\n");
        rep_u32(rep, 0, BLOCK_ERR_IO);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
}

/* ── E5-S8: Entry point ─────────────────────────────────────────────────── */
void block_pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    block_pd_pd_init();
    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    /* Dispatch all opcodes through the generic handler */
    sel4_server_register(&srv, SEL4_SERVER_OPCODE_ANY, block_pd_h_dispatch, (void *)0);
    sel4_server_run(&srv);
}
