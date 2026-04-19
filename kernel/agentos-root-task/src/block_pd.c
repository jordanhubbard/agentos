/*
 * block_pd.c — agentOS Generic Block Protection Domain
 *
 * OS-neutral IPC block device service. Provides OPEN/CLOSE/READ/WRITE/FLUSH/STATUS/TRIM
 * over seL4 IPC with LBA-addressed sector I/O via shared memory.
 * Up to BLOCK_MAX_HANDLES simultaneous partition claims.
 *
 * IPC Protocol (caller -> block_pd, channel CH_BLOCK_PD):
 *   MSG_BLOCK_OPEN   (0x1020) — claim partition → handle
 *   MSG_BLOCK_CLOSE  (0x1021) — release handle
 *   MSG_BLOCK_READ   (0x1022) — MR1=handle MR2=lba_lo MR3=lba_hi MR4=sectors → data in shmem
 *   MSG_BLOCK_WRITE  (0x1023) — MR1=handle MR2=lba_lo MR3=lba_hi MR4=sectors → ok
 *   MSG_BLOCK_FLUSH  (0x1024) — MR1=handle → ok
 *   MSG_BLOCK_STATUS (0x1025) — MR1=handle → MR1=sector_count MR2=sector_sz MR3=ro
 *   MSG_BLOCK_TRIM   (0x1026) — MR1=handle MR2=lba_lo MR3=lba_hi MR4=sectors → ok
 *
 * Hardware: virtio-blk MMIO or NVMe via device capability.
 *           Descriptor ring management is Phase 2.5 (see TODO below).
 *
 * Priority: 210
 */
#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/block_contract.h"

/* ── State ───────────────────────────────────────────────────────────────── */
static bool     s_handle_open[BLOCK_MAX_HANDLES];
static uint32_t s_dev_index[BLOCK_MAX_HANDLES];
static uint32_t s_partition[BLOCK_MAX_HANDLES];
static uint32_t s_sector_count_lo[BLOCK_MAX_HANDLES];
static uint32_t s_sector_count_hi[BLOCK_MAX_HANDLES];
static uint32_t s_read_only[BLOCK_MAX_HANDLES];

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void)
{
    for (uint32_t i = 0; i < BLOCK_MAX_HANDLES; i++) {
        s_handle_open[i]     = false;
        s_dev_index[i]       = 0;
        s_partition[i]       = 0;
        s_sector_count_lo[i] = 0;
        s_sector_count_hi[i] = 0;
        s_read_only[i]       = 0;
    }
    microkit_dbg_puts("[block_pd] ready\n");
}

void notified(microkit_channel ch) { (void)ch; }

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch; (void)msginfo;
    uint32_t op     = (uint32_t)microkit_mr_get(0);
    uint32_t handle = (uint32_t)microkit_mr_get(1);

    switch (op) {
    case MSG_BLOCK_OPEN: {
        uint32_t dev  = handle; /* MR1 = dev_index on OPEN */
        uint32_t part = (uint32_t)microkit_mr_get(2);
        uint32_t found = BLOCK_MAX_HANDLES;
        for (uint32_t i = 0; i < BLOCK_MAX_HANDLES; i++) {
            if (!s_handle_open[i]) { found = i; break; }
        }
        if (found == BLOCK_MAX_HANDLES) {
            microkit_mr_set(0, BLOCK_ERR_NO_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        s_handle_open[found]     = true;
        s_dev_index[found]       = dev;
        s_partition[found]       = part;
        /* TODO Phase 2.5: query virtio-blk config for real sector count */
        s_sector_count_lo[found] = 0x00200000u; /* placeholder: 1GB / 512 */
        s_sector_count_hi[found] = 0;
        s_read_only[found]       = 0;
        microkit_mr_set(0, BLOCK_OK);
        microkit_mr_set(1, found);
        return microkit_msginfo_new(0, 2);
    }
    case MSG_BLOCK_CLOSE:
        if (handle >= BLOCK_MAX_HANDLES || !s_handle_open[handle]) {
            microkit_mr_set(0, BLOCK_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        s_handle_open[handle] = false;
        microkit_mr_set(0, BLOCK_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_BLOCK_READ: {
        if (handle >= BLOCK_MAX_HANDLES || !s_handle_open[handle]) {
            microkit_mr_set(0, BLOCK_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        uint32_t sectors = (uint32_t)microkit_mr_get(4);
        if (sectors > BLOCK_MAX_SECTORS) {
            microkit_mr_set(0, BLOCK_ERR_TOO_MANY);
            return microkit_msginfo_new(0, 1);
        }
        /* TODO Phase 2.5: submit virtio-blk READ descriptor, wait, copy to block_shmem */
        microkit_mr_set(0, BLOCK_OK);
        microkit_mr_set(1, sectors);
        return microkit_msginfo_new(0, 2);
    }
    case MSG_BLOCK_WRITE: {
        if (handle >= BLOCK_MAX_HANDLES || !s_handle_open[handle]) {
            microkit_mr_set(0, BLOCK_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        if (s_read_only[handle]) {
            microkit_mr_set(0, BLOCK_ERR_READ_ONLY);
            return microkit_msginfo_new(0, 1);
        }
        uint32_t sectors = (uint32_t)microkit_mr_get(4);
        if (sectors > BLOCK_MAX_SECTORS) {
            microkit_mr_set(0, BLOCK_ERR_TOO_MANY);
            return microkit_msginfo_new(0, 1);
        }
        /* TODO Phase 2.5: copy from block_shmem, submit virtio-blk WRITE descriptor */
        microkit_mr_set(0, BLOCK_OK);
        microkit_mr_set(1, sectors);
        return microkit_msginfo_new(0, 2);
    }
    case MSG_BLOCK_FLUSH:
        if (handle >= BLOCK_MAX_HANDLES || !s_handle_open[handle]) {
            microkit_mr_set(0, BLOCK_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        /* TODO Phase 2.5: issue virtio-blk FLUSH command */
        microkit_mr_set(0, BLOCK_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_BLOCK_STATUS:
        if (handle >= BLOCK_MAX_HANDLES || !s_handle_open[handle]) {
            microkit_mr_set(0, BLOCK_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        microkit_mr_set(0, BLOCK_OK);
        microkit_mr_set(1, s_sector_count_lo[handle]);
        microkit_mr_set(2, s_sector_count_hi[handle]);
        microkit_mr_set(3, BLOCK_SECTOR_SIZE);
        microkit_mr_set(4, s_read_only[handle]);
        return microkit_msginfo_new(0, 5);
    case MSG_BLOCK_TRIM: {
        if (handle >= BLOCK_MAX_HANDLES || !s_handle_open[handle]) {
            microkit_mr_set(0, BLOCK_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        /* TODO Phase 2.5: issue virtio-blk DISCARD command for the LBA range */
        microkit_mr_set(0, BLOCK_OK);
        return microkit_msginfo_new(0, 1);
    }
    default:
        microkit_dbg_puts("[block_pd] unknown op\n");
        microkit_mr_set(0, BLOCK_ERR_NOT_IMPL);
        return microkit_msginfo_new(0, 1);
    }
}
