/*
 * ext2fs.c — ext2 filesystem protection domain for agentOS
 *
 * HURD-parity Track N: persistent filesystem over virtio_blk.
 *
 * Reads/writes blocks via OP_BLK_READ / OP_BLK_WRITE to the virtio_blk PD
 * (channel EXT2_CH_VIRTIO_BLK, local id=1).  Exposes a filesystem API to
 * the controller via PPCs on local channel id=0.
 *
 * Data exchange with virtio_blk goes through ext2_blk_dma_shmem (32 KB,
 * shared with virtio_blk, mapped at 0x4000000 in this PD and 0x5500000
 * in virtio_blk).
 *
 * Large I/O payloads (stat paths, read data, readdir results) flow through
 * ext2_shmem (64 KB, mapped at 0x3400000 in this PD and r-only in controller).
 *
 * Channels (ext2fs local view):
 *   id=0: controller     (receives PPCs)
 *   id=1: virtio_blk     (PPCs OUT for block I/O)
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "virtio_blk.h"
#include <stdbool.h>
#include <stdint.h>

/* ── Microkit setvars (filled by linker) ─────────────────────────────────── */
uintptr_t ext2_shmem_vaddr;          /* 64 KB I/O shmem, 0x3400000         */
uintptr_t ext2_blk_dma_shmem_vaddr;  /* 32 KB DMA shmem shared w/ virtio_blk */
uintptr_t log_drain_rings_vaddr;       /* console ring region (agentos.h)    */

/* ── Channel IDs (local to ext2fs) ─────────────────────────────────────── */
#define EXT2_CH_CTRL        0   /* controller PPCs in  */
#define EXT2_CH_VIRTIO_BLK  1   /* ext2fs PPCs out to virtio_blk */

/* ── Shmem sizes ─────────────────────────────────────────────────────────── */
#define EXT2_SHMEM_SIZE      0x10000u   /* 64 KB */
#define EXT2_BLK_DMA_SIZE    0x8000u    /* 32 KB */

/* ── ext2 on-disk structures ─────────────────────────────────────────────── */

typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;   /* block_size = 1024 << s_log_block_size */
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t _pad[5];
    uint16_t s_magic;            /* must be 0xEF53 */
    /* remaining fields unused in Phase 1 */
} __attribute__((packed)) ext2_superblock_t;

typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t _pad;
    uint32_t _reserved[3];
} __attribute__((packed)) ext2_bgd_t;

typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t _osd1;
    uint32_t i_block[15];   /* 12 direct + indirect + dbl-indirect + tri-indirect */
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  _osd2[12];
} __attribute__((packed)) ext2_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[255];
} __attribute__((packed)) ext2_dirent_t;

/* ── Static filesystem state ─────────────────────────────────────────────── */
static ext2_superblock_t  g_sb;
static bool               g_mounted    = false;
static uint32_t           g_block_size = 1024;

/* ── 8-entry LRU block cache ─────────────────────────────────────────────── */
#define BLOCK_CACHE_SIZE 8
static struct {
    uint32_t block_num;
    bool     valid;
    uint8_t  data[4096];   /* max block size 4 KB */
} g_cache[BLOCK_CACHE_SIZE];
static uint32_t g_cache_lru[BLOCK_CACHE_SIZE];
static uint32_t g_lru_clock = 0;

/* ── Bare-metal helpers ──────────────────────────────────────────────────── */

static void ext2_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static void ext2_memset(void *dst, uint8_t val, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = val;
}

static uint32_t ext2_strlen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static int ext2_strncmp(const char *a, const char *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

/* Minimal decimal formatter for log messages — writes to buf, returns len. */
static uint32_t ext2_u32toa(uint32_t v, char *buf)
{
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[12]; uint32_t i = 0;
    while (v) { tmp[i++] = '0' + (v % 10); v /= 10; }
    uint32_t len = i;
    for (uint32_t j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[len] = '\0';
    return len;
}

/* Simple debug log: writes literal string via microkit_dbg_puts */
static void ext2_log(const char *msg)
{
    microkit_dbg_puts(msg);
}

/* ── Block I/O via virtio_blk ────────────────────────────────────────────── */

/*
 * raw_blk_read — send OP_BLK_READ PPC to virtio_blk.
 * Data arrives in ext2_blk_dma_shmem_vaddr (shared DMA region).
 * sectors = g_block_size / 512; first_sector = block_num * sectors.
 * Returns 0 on success, non-zero on error.
 */
static int raw_blk_read(uint32_t block_num)
{
    uint32_t sectors_per_block = g_block_size >> 9;   /* / 512 */
    if (sectors_per_block == 0) sectors_per_block = 2; /* 1024-byte default */
    uint64_t first_sector = (uint64_t)block_num * sectors_per_block;

    microkit_mr_set(0, OP_BLK_READ);
    microkit_mr_set(1, (uint32_t)(first_sector & 0xFFFFFFFFu));
    microkit_mr_set(2, (uint32_t)(first_sector >> 32));
    microkit_mr_set(3, sectors_per_block);

    microkit_msginfo reply = microkit_ppcall(EXT2_CH_VIRTIO_BLK,
                                             microkit_msginfo_new(OP_BLK_READ, 4));
    uint32_t rc = (uint32_t)microkit_msginfo_get_label(reply);
    return (rc == 0) ? 0 : -1;   /* BLK_OK == 0 */
}

/*
 * blk_read_cached — return pointer to cached block data.
 * Evicts the LRU slot when the cache is full.
 */
static int blk_read_cached(uint32_t block_num, uint8_t **out)
{
    /* Lookup */
    for (uint32_t i = 0; i < BLOCK_CACHE_SIZE; i++) {
        if (g_cache[i].valid && g_cache[i].block_num == block_num) {
            g_cache_lru[i] = ++g_lru_clock;
            *out = g_cache[i].data;
            return 0;
        }
    }

    /* Find LRU victim */
    uint32_t victim = 0;
    uint32_t oldest = g_cache_lru[0];
    for (uint32_t i = 1; i < BLOCK_CACHE_SIZE; i++) {
        if (!g_cache[i].valid) { victim = i; oldest = 0; break; }
        if (g_cache_lru[i] < oldest) { oldest = g_cache_lru[i]; victim = i; }
    }

    /* Issue I/O — data lands in ext2_blk_dma_shmem_vaddr */
    int rc = raw_blk_read(block_num);
    if (rc != 0 || !ext2_blk_dma_shmem_vaddr) {
        /* I/O failure or DMA region not mapped — zero-fill for graceful degradation */
        ext2_memset(g_cache[victim].data, 0, sizeof(g_cache[victim].data));
    } else {
        uint32_t copy_size = g_block_size;
        if (copy_size > sizeof(g_cache[victim].data))
            copy_size = (uint32_t)sizeof(g_cache[victim].data);
        ext2_memcpy(g_cache[victim].data,
                    (const uint8_t *)ext2_blk_dma_shmem_vaddr,
                    copy_size);
    }

    g_cache[victim].block_num = block_num;
    g_cache[victim].valid     = true;
    g_cache_lru[victim]       = ++g_lru_clock;

    *out = g_cache[victim].data;
    return 0;
}

/* ── ext2 filesystem helpers ─────────────────────────────────────────────── */

/*
 * ext2_read_inode — populate *out from the on-disk inode table.
 * Inode numbering is 1-based.
 */
static int ext2_read_inode(uint32_t ino, ext2_inode_t *out)
{
    if (ino == 0) return -1;

    uint32_t group = (ino - 1) / g_sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % g_sb.s_inodes_per_group;

    /* Read the block group descriptor table.
     * BGD table starts in the block after the superblock. */
    uint32_t bgd_block  = g_sb.s_first_data_block + 1;
    uint32_t bgds_per_block = g_block_size / (uint32_t)sizeof(ext2_bgd_t);
    uint32_t bgd_blk_off    = group / bgds_per_block;
    uint32_t bgd_idx        = group % bgds_per_block;

    uint8_t *blk_data;
    if (blk_read_cached(bgd_block + bgd_blk_off, &blk_data) != 0) return -1;

    ext2_bgd_t bgd;
    ext2_memcpy(&bgd,
                blk_data + bgd_idx * sizeof(ext2_bgd_t),
                sizeof(ext2_bgd_t));

    /* Locate the inode within the inode table */
    uint32_t inode_size     = 128;   /* Phase 1: assume rev0 128-byte inodes */
    uint32_t inodes_per_blk = g_block_size / inode_size;
    uint32_t inode_blk      = bgd.bg_inode_table + index / inodes_per_blk;
    uint32_t inode_off      = (index % inodes_per_blk) * inode_size;

    if (blk_read_cached(inode_blk, &blk_data) != 0) return -1;

    ext2_memcpy(out, blk_data + inode_off, sizeof(ext2_inode_t));
    return 0;
}

/*
 * ext2_block_to_lba — resolve logical file block number to on-disk block number.
 * Handles direct (0..11), single-indirect (12), and double-indirect (13).
 */
static uint32_t ext2_block_to_lba(const ext2_inode_t *inode, uint32_t bnum)
{
    uint32_t ptrs_per_block = g_block_size / 4;

    /* Direct blocks */
    if (bnum < 12) {
        return inode->i_block[bnum];
    }

    /* Single-indirect */
    bnum -= 12;
    if (bnum < ptrs_per_block) {
        uint8_t *blk_data;
        if (blk_read_cached(inode->i_block[12], &blk_data) != 0) return 0;
        uint32_t entry;
        ext2_memcpy(&entry, blk_data + bnum * 4, 4);
        return entry;
    }

    /* Double-indirect */
    bnum -= ptrs_per_block;
    if (bnum < ptrs_per_block * ptrs_per_block) {
        uint32_t l1_idx = bnum / ptrs_per_block;
        uint32_t l2_idx = bnum % ptrs_per_block;
        uint8_t *blk_data;

        if (blk_read_cached(inode->i_block[13], &blk_data) != 0) return 0;
        uint32_t l1_blk;
        ext2_memcpy(&l1_blk, blk_data + l1_idx * 4, 4);
        if (l1_blk == 0) return 0;

        if (blk_read_cached(l1_blk, &blk_data) != 0) return 0;
        uint32_t entry;
        ext2_memcpy(&entry, blk_data + l2_idx * 4, 4);
        return entry;
    }

    /* Triple-indirect not implemented in Phase 1 */
    return 0;
}

/*
 * ext2_find_dirent — search directory dir_ino for an entry named name.
 * Sets *out_ino to the matching inode number; returns 0 on success.
 */
static int ext2_find_dirent(uint32_t dir_ino, const char *name, uint32_t *out_ino)
{
    ext2_inode_t dir_inode;
    if (ext2_read_inode(dir_ino, &dir_inode) != 0) return -1;

    uint32_t name_len  = ext2_strlen(name);
    uint32_t file_size = dir_inode.i_size;
    uint32_t offset    = 0;

    while (offset < file_size) {
        uint32_t bnum     = offset / g_block_size;
        uint32_t blk_lba  = ext2_block_to_lba(&dir_inode, bnum);
        if (blk_lba == 0) break;

        uint8_t *blk_data;
        if (blk_read_cached(blk_lba, &blk_data) != 0) break;

        uint32_t blk_off = offset % g_block_size;

        while (blk_off < g_block_size && offset < file_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(blk_data + blk_off);

            if (de->rec_len == 0) break;   /* corrupt */

            if (de->inode != 0 && de->name_len == (uint8_t)name_len) {
                if (ext2_strncmp(de->name, name, name_len) == 0) {
                    *out_ino = de->inode;
                    return 0;
                }
            }

            blk_off += de->rec_len;
            offset  += de->rec_len;
        }

        /* Advance to next block boundary if we didn't break mid-block */
        uint32_t next_blk_start = (bnum + 1) * g_block_size;
        if (offset < next_blk_start) offset = next_blk_start;
    }

    return -1;  /* not found */
}

/* ── Mount helper (shared by init() and OP_EXT2_MOUNT) ─────────────────── */

static int do_mount(void)
{
    /* The ext2 superblock is at byte offset 1024 = block 1 for 1024-byte
     * blocks, which is LBA 2 for 512-byte sectors.
     * We read block 0 of the device (containing the superblock at offset 1024
     * within the 2-sector group), then copy from byte 1024. */

    /* Read sectors 2-3 (byte offset 1024) as a raw 1024-byte region.
     * We issue OP_BLK_READ for sector 2, count 2. */
    microkit_mr_set(0, OP_BLK_READ);
    microkit_mr_set(1, 2);   /* LBA 2 */
    microkit_mr_set(2, 0);   /* sector_hi */
    microkit_mr_set(3, 2);   /* 2 sectors = 1024 bytes */

    microkit_msginfo reply = microkit_ppcall(EXT2_CH_VIRTIO_BLK,
                                             microkit_msginfo_new(OP_BLK_READ, 4));
    uint32_t rc = (uint32_t)microkit_msginfo_get_label(reply);

    if (rc != 0 || !ext2_blk_dma_shmem_vaddr) {
        /* I/O failure — copy zeros so magic check fails cleanly */
        ext2_memset(&g_sb, 0, sizeof(g_sb));
    } else {
        ext2_memcpy(&g_sb,
                    (const uint8_t *)ext2_blk_dma_shmem_vaddr,
                    sizeof(g_sb));
    }

    if (g_sb.s_magic != 0xEF53u) {
        ext2_log("[ext2fs] mount failed: bad superblock magic\n");
        return -1;
    }

    g_block_size = 1024u << g_sb.s_log_block_size;
    if (g_block_size == 0 || g_block_size > 4096) g_block_size = 1024;

    /* Seed the block-0 cache entry for the superblock block */
    {
        uint8_t *dummy;
        blk_read_cached(0, &dummy);   /* warms slot; data read above */
    }

    g_mounted = true;

    ext2_log("[ext2fs] mounted: block_size=");
    { char buf[12]; ext2_u32toa(g_block_size, buf); ext2_log(buf); }
    ext2_log(" blocks=");
    { char buf[12]; ext2_u32toa(g_sb.s_blocks_count, buf); ext2_log(buf); }
    ext2_log(" inodes=");
    { char buf[12]; ext2_u32toa(g_sb.s_inodes_count, buf); ext2_log(buf); }
    ext2_log("\n");

    return 0;
}

/* ── Path walker ─────────────────────────────────────────────────────────── */

/*
 * ext2_walk_path — resolve absolute path to inode number.
 * path must be null-terminated and start with '/'.
 * Returns inode number on success, 0 on failure.
 */
static uint32_t ext2_walk_path(const char *path)
{
    uint32_t ino = 2;   /* root inode */

    if (!path || path[0] != '/') return 0;

    const char *p = path + 1;
    if (*p == '\0') return ino;   /* root itself */

    while (*p) {
        /* Extract next component */
        const char *start = p;
        while (*p && *p != '/') p++;
        uint32_t comp_len = (uint32_t)(p - start);
        if (comp_len == 0) { if (*p == '/') { p++; continue; } break; }

        /* Null-terminate component in a local buffer */
        char comp[256];
        if (comp_len >= sizeof(comp)) return 0;
        ext2_memcpy(comp, start, comp_len);
        comp[comp_len] = '\0';

        uint32_t next_ino;
        if (ext2_find_dirent(ino, comp, &next_ino) != 0) return 0;
        ino = next_ino;

        if (*p == '/') p++;
    }

    return ino;
}

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void)
{
    /* Zero all cache slots */
    for (uint32_t i = 0; i < BLOCK_CACHE_SIZE; i++) {
        g_cache[i].valid = false;
        g_cache[i].block_num = 0;
        g_cache_lru[i] = 0;
    }
    g_lru_clock  = 0;
    g_mounted    = false;
    g_block_size = 1024;

    ext2_log("[ext2fs] init: I/O shmem ");
    ext2_log(ext2_shmem_vaddr ? "mapped\n" : "NOT MAPPED\n");
    ext2_log("[ext2fs] init: DMA shmem ");
    ext2_log(ext2_blk_dma_shmem_vaddr ? "mapped\n" : "NOT MAPPED\n");

    /* Attempt auto-mount */
    if (do_mount() == 0) {
        ext2_log("[ext2fs] auto-mount OK\n");
    } else {
        ext2_log("[ext2fs] auto-mount skipped (no ext2 device)\n");
    }
}

void notified(microkit_channel ch)
{
    (void)ch;
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch;
    uint32_t op = (uint32_t)microkit_msginfo_get_label(msginfo);

    switch (op) {

    /* ── OP_EXT2_MOUNT ──────────────────────────────────────────────────── */
    case OP_EXT2_MOUNT: {
        /* Invalidate cache */
        for (uint32_t i = 0; i < BLOCK_CACHE_SIZE; i++)
            g_cache[i].valid = false;
        g_mounted = false;

        if (do_mount() != 0) {
            microkit_mr_set(0, 0xFFu);
            return microkit_msginfo_new(0, 1);
        }

        microkit_mr_set(0, 0u);
        microkit_mr_set(1, g_sb.s_blocks_count);
        microkit_mr_set(2, g_sb.s_inodes_count);
        return microkit_msginfo_new(0, 3);
    }

    /* ── OP_EXT2_STAT ───────────────────────────────────────────────────── */
    case OP_EXT2_STAT: {
        if (!g_mounted) {
            microkit_mr_set(0, 0xFBu);   /* EXT2_ERR_NOT_MOUNTED */
            return microkit_msginfo_new(0, 1);
        }
        if (!ext2_shmem_vaddr) {
            microkit_mr_set(0, 0xFAu);   /* EXT2_ERR_NO_SHMEM */
            return microkit_msginfo_new(0, 1);
        }

        const char *path = (const char *)ext2_shmem_vaddr;
        uint32_t ino = ext2_walk_path(path);
        if (ino == 0) {
            microkit_mr_set(0, 0xF9u);   /* EXT2_ERR_NOENT */
            return microkit_msginfo_new(0, 1);
        }

        ext2_inode_t inode;
        if (ext2_read_inode(ino, &inode) != 0) {
            microkit_mr_set(0, 0xF8u);   /* EXT2_ERR_IO */
            return microkit_msginfo_new(0, 1);
        }

        microkit_mr_set(0, 0u);
        microkit_mr_set(1, ino);
        microkit_mr_set(2, inode.i_size);
        microkit_mr_set(3, inode.i_mode);
        return microkit_msginfo_new(0, 4);
    }

    /* ── OP_EXT2_READ ───────────────────────────────────────────────────── */
    case OP_EXT2_READ: {
        if (!g_mounted) {
            microkit_mr_set(0, 0xFBu);
            return microkit_msginfo_new(0, 1);
        }
        if (!ext2_shmem_vaddr) {
            microkit_mr_set(0, 0xFAu);
            return microkit_msginfo_new(0, 1);
        }

        uint32_t ino        = (uint32_t)microkit_mr_get(1);
        uint32_t file_off   = (uint32_t)microkit_mr_get(2);
        uint32_t req_len    = (uint32_t)microkit_mr_get(3);

        if (req_len > EXT2_SHMEM_SIZE) req_len = EXT2_SHMEM_SIZE;

        ext2_inode_t inode;
        if (ext2_read_inode(ino, &inode) != 0) {
            microkit_mr_set(0, 0xF8u);
            return microkit_msginfo_new(0, 1);
        }

        if (file_off >= inode.i_size) {
            microkit_mr_set(0, 0u);
            microkit_mr_set(1, 0u);
            return microkit_msginfo_new(0, 2);
        }

        /* Clamp to file size */
        uint32_t avail = inode.i_size - file_off;
        if (req_len > avail) req_len = avail;

        uint8_t  *out_ptr  = (uint8_t *)ext2_shmem_vaddr;
        uint32_t  written  = 0;
        uint32_t  cur_off  = file_off;

        while (written < req_len) {
            uint32_t bnum    = cur_off / g_block_size;
            uint32_t boff    = cur_off % g_block_size;
            uint32_t blk_lba = ext2_block_to_lba(&inode, bnum);

            if (blk_lba == 0) break;   /* sparse / unallocated block */

            uint8_t *blk_data;
            if (blk_read_cached(blk_lba, &blk_data) != 0) break;

            uint32_t chunk = g_block_size - boff;
            if (chunk > req_len - written) chunk = req_len - written;

            ext2_memcpy(out_ptr + written, blk_data + boff, chunk);
            written  += chunk;
            cur_off  += chunk;
        }

        microkit_mr_set(0, 0u);
        microkit_mr_set(1, written);
        return microkit_msginfo_new(0, 2);
    }

    /* ── OP_EXT2_WRITE ──────────────────────────────────────────────────── */
    case OP_EXT2_WRITE: {
        /* Phase 1: read-only filesystem */
        microkit_mr_set(0, 0xFEu);   /* EXT2_ERR_READONLY */
        return microkit_msginfo_new(0, 1);
    }

    /* ── OP_EXT2_READDIR ────────────────────────────────────────────────── */
    case OP_EXT2_READDIR: {
        if (!g_mounted) {
            microkit_mr_set(0, 0xFBu);
            return microkit_msginfo_new(0, 1);
        }
        if (!ext2_shmem_vaddr) {
            microkit_mr_set(0, 0xFAu);
            return microkit_msginfo_new(0, 1);
        }

        uint32_t dir_ino = (uint32_t)microkit_mr_get(1);

        ext2_inode_t dir_inode;
        if (ext2_read_inode(dir_ino, &dir_inode) != 0) {
            microkit_mr_set(0, 0xF8u);
            return microkit_msginfo_new(0, 1);
        }

        uint8_t  *out_ptr    = (uint8_t *)ext2_shmem_vaddr;
        uint32_t  out_limit  = EXT2_SHMEM_SIZE;
        uint32_t  out_used   = 0;
        uint32_t  entry_count = 0;
        uint32_t  file_size  = dir_inode.i_size;
        uint32_t  offset     = 0;

        while (offset < file_size) {
            uint32_t bnum    = offset / g_block_size;
            uint32_t blk_lba = ext2_block_to_lba(&dir_inode, bnum);
            if (blk_lba == 0) break;

            uint8_t *blk_data;
            if (blk_read_cached(blk_lba, &blk_data) != 0) break;

            uint32_t blk_off = offset % g_block_size;

            while (blk_off < g_block_size && offset < file_size) {
                ext2_dirent_t *de = (ext2_dirent_t *)(blk_data + blk_off);
                if (de->rec_len == 0) goto readdir_done;

                if (de->inode != 0) {
                    /* Write a packed entry: inode(4) + name_len(1) + name */
                    uint32_t name_len = de->name_len;
                    uint32_t needed   = 4 + 1 + name_len + 1; /* +1 null term */
                    if (out_used + needed > out_limit) goto readdir_done;

                    /* inode number (LE32) */
                    out_ptr[out_used + 0] = (uint8_t)(de->inode        & 0xFF);
                    out_ptr[out_used + 1] = (uint8_t)((de->inode >> 8)  & 0xFF);
                    out_ptr[out_used + 2] = (uint8_t)((de->inode >> 16) & 0xFF);
                    out_ptr[out_used + 3] = (uint8_t)((de->inode >> 24) & 0xFF);
                    /* name_len */
                    out_ptr[out_used + 4] = (uint8_t)name_len;
                    /* name + null */
                    ext2_memcpy(out_ptr + out_used + 5, de->name, name_len);
                    out_ptr[out_used + 5 + name_len] = '\0';

                    out_used += needed;
                    entry_count++;
                }

                blk_off += de->rec_len;
                offset  += de->rec_len;
            }

            uint32_t next_blk_start = (bnum + 1) * g_block_size;
            if (offset < next_blk_start) offset = next_blk_start;
        }

    readdir_done:
        microkit_mr_set(0, 0u);
        microkit_mr_set(1, entry_count);
        return microkit_msginfo_new(0, 2);
    }

    /* ── OP_EXT2_STATUS ─────────────────────────────────────────────────── */
    case OP_EXT2_STATUS: {
        microkit_mr_set(0, 0u);
        microkit_mr_set(1, g_mounted ? 1u : 0u);
        microkit_mr_set(2, g_sb.s_blocks_count);
        microkit_mr_set(3, g_sb.s_free_blocks_count);
        return microkit_msginfo_new(0, 4);
    }

    /* ── Unknown opcode ─────────────────────────────────────────────────── */
    default: {
        ext2_log("[ext2fs] WARNING: unknown opcode\n");
        microkit_mr_set(0, 0xFFu);
        return microkit_msginfo_new(0xFFFF, 1);
    }

    } /* switch (op) */
}
