/*
 * agentOS VFS Server Protection Domain
 *
 * Provides a POSIX-flavoured virtual filesystem to all other PDs.
 * Two backends:
 *   MEM  — fully in-memory, always available, static allocation only.
 *   BLK  — thin wrapper over virtio-blk (CH12); stubbed if absent at init.
 *
 * All callers use PPCs.  Large data (read/write payloads, paths, directory
 * listings) flows through the vfs_io_shmem region — NOT through MRs.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "vfs.h"

/* ── Microkit setvar_vaddr — filled in by the system initialiser ─────────── */
uintptr_t vfs_io_shmem_vaddr;             /* 128 KB rw, mapped at 0x5000000 */
uintptr_t blk_dma_shmem_vaddr;           /* virtio-blk DMA region, 0x6000000 */
uintptr_t spawn_elf_shmem_vfs_vaddr;     /* 512 KB ELF staging, 0x7000000 */
uintptr_t log_drain_rings_vaddr;           /* console ring region (required by agentos.h) */

/* ── Shmem access helpers ─────────────────────────────────────────────────── */
#define SHMEM_PATH()    ((char *)(vfs_io_shmem_vaddr + VFS_SHMEM_PATH_OFF))
#define SHMEM_READDIR() ((char *)(vfs_io_shmem_vaddr + VFS_SHMEM_READDIR_OFF))
#define SHMEM_DATA()    ((uint8_t *)(vfs_io_shmem_vaddr + VFS_SHMEM_DATA_OFF))

/* ── Static state ─────────────────────────────────────────────────────────── */
static vfs_handle_t  handles[VFS_MAX_HANDLES];
static mem_inode_t   mem_inodes[VFS_MEM_MAX_INODES];
static vfs_mount_t   mounts[VFS_MAX_MOUNTS];
static uint32_t      open_handle_count = 0;

/* Set when OP_BLK_INFO succeeds at init; blk ops are no-ops otherwise. */
static bool          blk_present = false;
static uint64_t      blk_block_count = 0;   /* total blocks on device */
static uint32_t      blk_block_size  = 0;   /* bytes per block */

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Bare-metal strlen — available via m3_bare_metal.c, declared here for clarity */
static uint32_t vfs_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

/* Safe strncpy — always null-terminates dst[0..len-1] */
static void vfs_strncpy(char *dst, const char *src, uint32_t len) {
    uint32_t i;
    for (i = 0; i < len - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* strcmp — returns 0 on equal */
static int vfs_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Allocate a free handle slot; returns VFS_MAX_HANDLES on failure */
static uint32_t alloc_handle(void) {
    for (uint32_t i = 0; i < VFS_MAX_HANDLES; i++) {
        if (!handles[i].active) return i;
    }
    return VFS_MAX_HANDLES;
}

/* Path basename — returns pointer to final component within path */
static const char *vfs_basename(const char *path) {
    const char *last = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' && *(p + 1)) last = p + 1;
    }
    return last;
}

/*
 * vfs_normalize_path — resolve ".." components, collapse multiple slashes.
 *
 * Walks the input path segment-by-segment using a segment stack.  A ".."
 * pops the top segment; a "." is skipped.  The result is written into
 * out[0..out_sz-1] with a leading '/' and null terminator.
 *
 * Returns false if the result would escape the root (more ".." than path
 * segments) or if out_sz is too small.  Callers must treat a false return
 * as an error and not attempt the path lookup.
 */
static bool vfs_normalize_path(const char *in, char *out, uint32_t out_sz)
{
    /* Segment stack: each entry is the start offset of a segment in `out`.
     * Maximum path depth matches the parent-walk limit below (16 levels). */
#define VFS_MAX_DEPTH 16
    uint32_t seg_start[VFS_MAX_DEPTH];
    int      depth = 0;

    if (!in || in[0] != '/') return false;
    if (out_sz < 2) return false;

    uint32_t pos = 0;
    out[pos++] = '/';   /* always start with root slash */

    const char *p = in + 1;   /* skip leading '/' */

    while (*p) {
        /* Skip extra slashes */
        if (*p == '/') { p++; continue; }

        /* Find end of this segment */
        const char *seg_end = p;
        while (*seg_end && *seg_end != '/') seg_end++;
        uint32_t seg_len = (uint32_t)(seg_end - p);

        if (seg_len == 1 && p[0] == '.') {
            /* "." — stay in current directory */
            p = seg_end;
            continue;
        }

        if (seg_len == 2 && p[0] == '.' && p[1] == '.') {
            /* ".." — go up one level */
            if (depth == 0) {
                /* Already at root — would escape, reject */
                return false;
            }
            depth--;
            /* Rewind pos to the start of the popped segment */
            pos = seg_start[depth];
            p = seg_end;
            continue;
        }

        /* Normal segment — check stack space and output buffer space */
        if (depth >= VFS_MAX_DEPTH) return false;
        if (pos + seg_len + 2 > out_sz) return false;  /* +2: '/' + NUL */

        /* Record where this segment starts in out */
        seg_start[depth] = pos;
        depth++;

        /* Append separator (not before root or first segment) */
        if (pos > 1) out[pos++] = '/';

        for (uint32_t k = 0; k < seg_len; k++)
            out[pos++] = p[k];

        p = seg_end;
    }

    out[pos] = '\0';
    return true;
#undef VFS_MAX_DEPTH
}

/*
 * Find the inode for an exact path.  We normalise the path first to
 * resolve ".." and collapse slashes; then do a simple linear scan
 * reconstructing each inode's full path by walking parent_ino.
 *
 * The parent-inode walk includes cycle detection to guard against
 * corrupted inode tables (a cycle would otherwise cause an infinite loop).
 *
 * Returns inode index, or VFS_MEM_MAX_INODES if not found or on error.
 */
static uint32_t mem_find_inode(const char *path) {
    /* Normalise the incoming path */
    char norm[256];
    if (!vfs_normalize_path(path, norm, sizeof(norm))) {
        return VFS_MEM_MAX_INODES;
    }
    path = norm;

    /* Root directory is always inode 0 */
    if (path[0] == '/' && path[1] == '\0') return 0;

    for (uint32_t i = 0; i < VFS_MEM_MAX_INODES; i++) {
        if (!mem_inodes[i].active) continue;
        if (i == 0) continue; /* root handled above */

        /* Build the full path of inode i by walking parent chain.
         * Cycle detection: track visited inodes in a bool array. */
        char     full[256];
        uint32_t segments[16];   /* inode indices from root to i */
        uint32_t depth = 0;
        uint32_t cur   = i;

        bool visited[VFS_MEM_MAX_INODES];
        for (uint32_t v = 0; v < VFS_MEM_MAX_INODES; v++) visited[v] = false;

        while (cur != 0 && depth < 16) {
            if (visited[cur]) {
                /* Cycle detected in parent-inode chain — skip this inode */
                log_drain_write(VFS_CONSOLE_SLOT, VFS_PD_ID,
                            "[vfs] cycle detected in inode parent chain\n");
                depth = 0;  /* signal invalid chain */
                break;
            }
            visited[cur] = true;
            segments[depth++] = cur;
            cur = mem_inodes[cur].parent_ino;
        }

        if (depth == 0) continue;  /* cycle or empty chain */

        /* Reconstruct path string */
        uint32_t pos = 0;
        full[pos++] = '/';
        for (int d = (int)depth - 1; d >= 0; d--) {
            const char *n = mem_inodes[segments[d]].name;
            for (uint32_t k = 0; n[k] && pos < 255; k++)
                full[pos++] = n[k];
            if (d > 0 && pos < 255)
                full[pos++] = '/';
        }
        full[pos] = '\0';

        if (vfs_strcmp(full, path) == 0) return i;
    }
    return VFS_MEM_MAX_INODES;
}

/* Allocate a new inode slot; returns VFS_MEM_MAX_INODES on failure */
static uint32_t mem_alloc_inode(void) {
    /* Skip inode 0 (reserved for root) */
    for (uint32_t i = 1; i < VFS_MEM_MAX_INODES; i++) {
        if (!mem_inodes[i].active) return i;
    }
    return VFS_MEM_MAX_INODES;
}

/*
 * Add a child name to a directory inode's data area.
 * Directory data is a sequence of null-terminated name strings.
 * An empty directory starts with a single NUL byte.
 *
 * Returns true on success.
 */
static bool mem_dir_add_child(uint32_t dir_ino, const char *name) {
    mem_inode_t *dir = &mem_inodes[dir_ino];
    uint32_t nlen = vfs_strlen(name) + 1;  /* include NUL */

    /* Find end of existing entries (stop at first NUL that is also at pos 0
     * of a potential new entry — we track position explicitly). */
    uint32_t pos = 0;
    while (pos < dir->size) {
        /* Skip over one null-terminated string */
        uint32_t start = pos;
        while (pos < dir->size && dir->data[pos]) pos++;
        pos++; /* skip the NUL */
        (void)start;
    }

    if (pos + nlen > VFS_MEM_FILE_SIZE) return false;

    for (uint32_t i = 0; i < nlen; i++)
        dir->data[pos + i] = (uint8_t)name[i];
    dir->size = pos + nlen;
    return true;
}

/*
 * Remove a child name from a directory inode's data area.
 * Compacts the remaining entries in place.
 */
static void mem_dir_remove_child(uint32_t dir_ino, const char *name) {
    mem_inode_t *dir = &mem_inodes[dir_ino];
    uint32_t pos = 0;

    while (pos < dir->size) {
        uint32_t entry_start = pos;
        /* measure entry length */
        while (pos < dir->size && dir->data[pos]) pos++;
        pos++; /* skip NUL */
        uint32_t entry_end = pos;

        const char *entry = (const char *)&dir->data[entry_start];
        if (vfs_strcmp(entry, name) == 0) {
            /* Shift remaining data left */
            uint32_t tail = dir->size - entry_end;
            for (uint32_t i = 0; i < tail; i++)
                dir->data[entry_start + i] = dir->data[entry_end + i];
            dir->size -= (entry_end - entry_start);
            return;
        }
    }
}

/* Resolve which backend handles a given path, using the mount table. */
static uint8_t path_to_backend(const char *path) {
    /* Find the longest matching prefix */
    uint32_t best_len    = 0;
    uint8_t  best_type   = VFS_BACKEND_MEM;

    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].active) continue;
        uint32_t plen = vfs_strlen(mounts[i].prefix);
        if (plen < best_len) continue;

        /* Check prefix match */
        bool match = true;
        for (uint32_t k = 0; k < plen; k++) {
            if (mounts[i].prefix[k] != path[k]) { match = false; break; }
        }
        if (!match) continue;

        /* The next char in path must be '/' or '\0' (unless prefix is "/") */
        if (plen > 1 && path[plen] != '/' && path[plen] != '\0') continue;

        best_len  = plen;
        best_type = mounts[i].backend_type;
    }
    return best_type;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MemBackend operations
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t mem_open(const char *path, uint32_t flags) {
    IPC_STUB_LOCALS
    /* Ensure path starts with '/' */
    if (path[0] != '/') return VFS_ERR_INVAL;

    uint32_t ino = mem_find_inode(path);

    if (ino == VFS_MEM_MAX_INODES) {
        /* Not found — create if O_CREAT */
        if (!(flags & VFS_O_CREAT)) return VFS_ERR_NOT_FOUND;

        /* Find parent directory */
        const char *base = vfs_basename(path);
        uint32_t    base_off = (uint32_t)(base - path);
        char parent_path[256];
        if (base_off <= 1) {
            parent_path[0] = '/';
            parent_path[1] = '\0';
        } else {
            uint32_t plen = base_off - 1;  /* strip trailing slash */
            for (uint32_t k = 0; k < plen && k < 255; k++)
                parent_path[k] = path[k];
            parent_path[base_off - 1] = '\0';
        }

        uint32_t parent_ino = mem_find_inode(parent_path);
        if (parent_ino == VFS_MEM_MAX_INODES) return VFS_ERR_NOT_FOUND;
        if (!mem_inodes[parent_ino].is_dir)    return VFS_ERR_NOT_DIR;

        ino = mem_alloc_inode();
        if (ino == VFS_MEM_MAX_INODES) return VFS_ERR_NO_SPACE;

        mem_inode_t *n = &mem_inodes[ino];
        n->active     = true;
        n->is_dir     = (flags & VFS_O_DIR) ? true : false;
        n->parent_ino = parent_ino;
        n->size       = 0;
        /* memset data area to 0 */
        for (uint32_t k = 0; k < VFS_MEM_FILE_SIZE; k++) n->data[k] = 0;
        vfs_strncpy(n->name, base, VFS_MEM_NAME_MAX);

        if (!mem_dir_add_child(parent_ino, base)) {
            n->active = false;
            return VFS_ERR_NO_SPACE;
        }

        log_drain_write(VFS_CONSOLE_SLOT, VFS_PD_ID, "[vfs] created inode\n");
    } else {
        /* Exists — check dir/file consistency */
        if ((flags & VFS_O_DIR) && !mem_inodes[ino].is_dir)
            return VFS_ERR_NOT_DIR;
    }

    uint32_t h = alloc_handle();
    if (h == VFS_MAX_HANDLES) return VFS_ERR_NO_HANDLES;

    handles[h].active  = true;
    handles[h].backend = VFS_BACKEND_MEM;
    handles[h].inode   = ino;
    handles[h].flags   = flags;
    handles[h].offset  = (flags & VFS_O_APPEND) ? mem_inodes[ino].size : 0;
    open_handle_count++;

    /* Encode both result and handle: caller checks MR0==VFS_OK, MR1==handle */
    rep_u32(rep, 0, VFS_OK);
    rep_u32(rep, 4, h);
    return VFS_OK;
}

static uint32_t mem_close(uint32_t h) {
    IPC_STUB_LOCALS
    if (h >= VFS_MAX_HANDLES || !handles[h].active) return VFS_ERR_INVAL;
    handles[h].active = false;
    open_handle_count--;
    rep_u32(rep, 0, VFS_OK);
    return VFS_OK;
}

static uint32_t mem_read(uint32_t h, uint64_t offset, uint32_t length) {
    IPC_STUB_LOCALS
    if (h >= VFS_MAX_HANDLES || !handles[h].active) return VFS_ERR_INVAL;
    if (!(handles[h].flags & VFS_O_RD))             return VFS_ERR_PERM;

    mem_inode_t *ino = &mem_inodes[handles[h].inode];
    if (ino->is_dir) return VFS_ERR_INVAL;

    if (offset >= ino->size) {
        rep_u32(rep, 0, VFS_OK);
        rep_u32(rep, 4, 0);
        return VFS_OK;
    }

    uint32_t avail = ino->size - (uint32_t)offset;
    uint32_t to_read = length < avail ? length : avail;
    if (to_read > VFS_SHMEM_DATA_MAX) to_read = (uint32_t)VFS_SHMEM_DATA_MAX;

    uint8_t *dst = SHMEM_DATA();
    const uint8_t *src = ino->data + offset;
    for (uint32_t i = 0; i < to_read; i++) dst[i] = src[i];

    rep_u32(rep, 0, VFS_OK);
    rep_u32(rep, 4, to_read);
    return VFS_OK;
}

static uint32_t mem_write(uint32_t h, uint64_t offset, uint32_t length) {
    IPC_STUB_LOCALS
    if (h >= VFS_MAX_HANDLES || !handles[h].active) return VFS_ERR_INVAL;
    if (!(handles[h].flags & VFS_O_WR))             return VFS_ERR_PERM;

    mem_inode_t *ino = &mem_inodes[handles[h].inode];
    if (ino->is_dir) return VFS_ERR_INVAL;

    /* Clamp to static file size limit */
    if (offset >= VFS_MEM_FILE_SIZE) return VFS_ERR_NO_SPACE;
    uint32_t avail   = VFS_MEM_FILE_SIZE - (uint32_t)offset;
    uint32_t to_write = length < avail ? length : avail;
    if (to_write > VFS_SHMEM_DATA_MAX) to_write = (uint32_t)VFS_SHMEM_DATA_MAX;

    const uint8_t *src = SHMEM_DATA();
    uint8_t *dst = ino->data + offset;
    for (uint32_t i = 0; i < to_write; i++) dst[i] = src[i];

    if ((uint32_t)offset + to_write > ino->size)
        ino->size = (uint32_t)offset + to_write;

    rep_u32(rep, 0, VFS_OK);
    rep_u32(rep, 4, to_write);
    return VFS_OK;
}

static uint32_t mem_stat(uint32_t h) {
    IPC_STUB_LOCALS
    if (h >= VFS_MAX_HANDLES || !handles[h].active) return VFS_ERR_INVAL;

    mem_inode_t *ino = &mem_inodes[handles[h].inode];
    uint32_t mode = ino->is_dir ? 0x4000u : 0x8000u;  /* S_IFDIR / S_IFREG */

    rep_u32(rep, 0, VFS_OK);
    rep_u32(rep, 4, (uintptr_t)(ino->size & 0xFFFFFFFFu));
    rep_u32(rep, 8, 0);  /* size_hi — mem files always < 4GB */
    rep_u32(rep, 12, mode);
    return VFS_OK;
}

static uint32_t mem_unlink(const char *path) {
    IPC_STUB_LOCALS
    uint32_t ino = mem_find_inode(path);
    if (ino == VFS_MEM_MAX_INODES) return VFS_ERR_NOT_FOUND;
    if (ino == 0)                   return VFS_ERR_PERM;  /* cannot unlink root */
    if (mem_inodes[ino].is_dir)     return VFS_ERR_PERM;  /* use rmdir (not implemented) */

    /* Check no open handles reference this inode */
    for (uint32_t i = 0; i < VFS_MAX_HANDLES; i++) {
        if (handles[i].active && handles[i].inode == ino)
            return VFS_ERR_PERM;
    }

    mem_dir_remove_child(mem_inodes[ino].parent_ino, mem_inodes[ino].name);
    mem_inodes[ino].active = false;
    mem_inodes[ino].size   = 0;

    rep_u32(rep, 0, VFS_OK);
    return VFS_OK;
}

static uint32_t mem_mkdir(const char *path) {
    IPC_STUB_LOCALS
    if (mem_find_inode(path) != VFS_MEM_MAX_INODES)
        return VFS_ERR_EXISTS;

    const char *base    = vfs_basename(path);
    uint32_t    base_off = (uint32_t)(base - path);
    char parent_path[256];

    if (base_off <= 1) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        uint32_t plen = base_off - 1;
        for (uint32_t k = 0; k < plen && k < 255; k++)
            parent_path[k] = path[k];
        parent_path[base_off - 1] = '\0';
    }

    uint32_t parent_ino = mem_find_inode(parent_path);
    if (parent_ino == VFS_MEM_MAX_INODES)           return VFS_ERR_NOT_FOUND;
    if (!mem_inodes[parent_ino].is_dir)              return VFS_ERR_NOT_DIR;

    uint32_t ino = mem_alloc_inode();
    if (ino == VFS_MEM_MAX_INODES) return VFS_ERR_NO_SPACE;

    mem_inode_t *n = &mem_inodes[ino];
    n->active     = true;
    n->is_dir     = true;
    n->parent_ino = parent_ino;
    n->size       = 0;
    for (uint32_t k = 0; k < VFS_MEM_FILE_SIZE; k++) n->data[k] = 0;
    vfs_strncpy(n->name, base, VFS_MEM_NAME_MAX);

    if (!mem_dir_add_child(parent_ino, base)) {
        n->active = false;
        return VFS_ERR_NO_SPACE;
    }

    log_drain_write(VFS_CONSOLE_SLOT, VFS_PD_ID, "[vfs] mkdir ok\n");
    rep_u32(rep, 0, VFS_OK);
    return VFS_OK;
}

/*
 * Enumerate the direct children of the directory at path.
 * Writes null-separated child names into shmem[304..815].
 * Returns the entry count in MR1.
 */
static uint32_t mem_readdir(const char *path) {
    IPC_STUB_LOCALS
    uint32_t dir_ino = mem_find_inode(path);
    if (dir_ino == VFS_MEM_MAX_INODES)          return VFS_ERR_NOT_FOUND;
    if (!mem_inodes[dir_ino].is_dir)             return VFS_ERR_NOT_DIR;

    char    *buf   = SHMEM_READDIR();
    uint32_t pos   = 0;
    uint32_t count = 0;

    /* Walk all inodes; emit direct children only */
    for (uint32_t i = 0; i < VFS_MEM_MAX_INODES; i++) {
        if (!mem_inodes[i].active) continue;
        if (i == dir_ino) continue;
        if (mem_inodes[i].parent_ino != dir_ino) continue;

        const char *name = mem_inodes[i].name;
        uint32_t nlen = vfs_strlen(name) + 1;

        if (pos + nlen > VFS_SHMEM_READDIR_MAX) break;
        for (uint32_t k = 0; k < nlen; k++)
            buf[pos + k] = name[k];  /* includes NUL */
        pos += nlen;
        count++;
    }

    rep_u32(rep, 0, VFS_OK);
    rep_u32(rep, 4, count);
    return VFS_OK;
}

static uint32_t mem_truncate(uint32_t h, uint64_t new_size) {
    IPC_STUB_LOCALS
    if (h >= VFS_MAX_HANDLES || !handles[h].active) return VFS_ERR_INVAL;
    if (!(handles[h].flags & VFS_O_WR))             return VFS_ERR_PERM;

    mem_inode_t *ino = &mem_inodes[handles[h].inode];
    if (ino->is_dir) return VFS_ERR_INVAL;
    if (new_size > VFS_MEM_FILE_SIZE) return VFS_ERR_NO_SPACE;

    uint32_t sz = (uint32_t)new_size;
    if (sz > ino->size) {
        /* Zero-extend */
        for (uint32_t k = ino->size; k < sz; k++) ino->data[k] = 0;
    }
    ino->size = sz;

    rep_u32(rep, 0, VFS_OK);
    return VFS_OK;
}

/* sync is a no-op for the mem backend — data is already "durable" in RAM */
static uint32_t mem_sync(uint32_t h) {
    IPC_STUB_LOCALS
    if (h >= VFS_MAX_HANDLES || !handles[h].active) return VFS_ERR_INVAL;
    rep_u32(rep, 0, VFS_OK);
    return VFS_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * BlkBackend — thin virtio-blk wrapper
 *
 * All reads/writes exchange data through blk_dma_shmem (mapped at
 * 0x6000000, shared with virtio-blk driver).  We translate byte offsets
 * to block numbers using blk_block_size determined at init.
 *
 * Individual files are not yet tracked per-block; the stub delegates each
 * I/O as a raw block operation.  A real implementation would maintain a
 * block allocation table in the first few sectors.
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t blk_call(uint32_t op, uint64_t block_lo_hi, uint32_t count) {
    IPC_STUB_LOCALS
    if (!blk_present) {
        rep_u32(rep, 0, VFS_ERR_IO);
        return VFS_ERR_IO;
    }
    rep_u32(rep, 0, op);
    rep_u32(rep, 4, (uintptr_t)(block_lo_hi & 0xFFFFFFFFu));
    rep_u32(rep, 8, (uintptr_t)(block_lo_hi >> 32));
    rep_u32(rep, 12, count);
    /* E5-S8: ppcall stubbed */
    uint32_t blk_rc = (uint32_t)msg_u32(req, 0);
    return (blk_rc == BLK_OK) ? VFS_OK : VFS_ERR_IO;
}

/*
 * blk_open — no per-file tracking; just validates the handle bookkeeping.
 * Inode number is repurposed as the block address base.
 */
static uint32_t blk_open(const char *path, uint32_t flags) {
    IPC_STUB_LOCALS
    if (!blk_present) {
        rep_u32(rep, 0, VFS_ERR_IO);
        rep_u32(rep, 4, 0);
        return VFS_ERR_IO;
    }

    /* Derive a pseudo-inode (block address base) from the path.
     * A production BLK VFS would read a superblock and directory here. */
    uint32_t pseudo_ino = 0;
    for (const char *p = path; *p; p++)
        pseudo_ino = pseudo_ino * 31u + (uint8_t)*p;
    pseudo_ino %= (uint32_t)(blk_block_count > 0 ? blk_block_count : 1);

    uint32_t h = alloc_handle();
    if (h == VFS_MAX_HANDLES) {
        rep_u32(rep, 0, VFS_ERR_NO_HANDLES);
        rep_u32(rep, 4, 0);
        return VFS_ERR_NO_HANDLES;
    }

    handles[h].active  = true;
    handles[h].backend = VFS_BACKEND_BLK;
    handles[h].inode   = pseudo_ino;
    handles[h].flags   = flags;
    handles[h].offset  = 0;
    open_handle_count++;

    rep_u32(rep, 0, VFS_OK);
    rep_u32(rep, 4, h);
    return VFS_OK;
}

static uint32_t blk_close(uint32_t h) {
    IPC_STUB_LOCALS
    if (h >= VFS_MAX_HANDLES || !handles[h].active) return VFS_ERR_INVAL;
    handles[h].active = false;
    open_handle_count--;
    rep_u32(rep, 0, VFS_OK);
    return VFS_OK;
}

/*
 * blk_read — issue OP_BLK_READ on CH12; data lands in blk_dma_shmem;
 * copy into the caller-visible vfs_io_shmem data buffer.
 */
static uint32_t blk_read(uint32_t h, uint64_t offset, uint32_t length) {
    IPC_STUB_LOCALS
    if (!blk_present)                               return VFS_ERR_IO;
    if (h >= VFS_MAX_HANDLES || !handles[h].active) return VFS_ERR_INVAL;
    if (blk_block_size == 0)                        return VFS_ERR_IO;

    uint32_t to_read = length < (uint32_t)VFS_SHMEM_DATA_MAX
                     ? length : (uint32_t)VFS_SHMEM_DATA_MAX;

    uint64_t first_block = offset / blk_block_size;
    uint32_t blk_count   = (to_read + blk_block_size - 1) / blk_block_size;

    uint32_t rc = blk_call(OP_BLK_READ, first_block, blk_count);
    if (rc != VFS_OK) {
        rep_u32(rep, 0, rc);
        rep_u32(rep, 4, 0);
        return rc;
    }

    /* Copy from DMA region to the vfs shmem data buffer */
    const uint8_t *src = (const uint8_t *)blk_dma_shmem_vaddr;
    uint8_t       *dst = SHMEM_DATA();
    for (uint32_t i = 0; i < to_read; i++) dst[i] = src[i];

    rep_u32(rep, 0, VFS_OK);
    rep_u32(rep, 4, to_read);
    return VFS_OK;
}

/*
 * blk_write — copy caller data from vfs_io_shmem into blk_dma_shmem,
 * then issue OP_BLK_WRITE on CH12.
 */
static uint32_t blk_write(uint32_t h, uint64_t offset, uint32_t length) {
    IPC_STUB_LOCALS
    if (!blk_present)                               return VFS_ERR_IO;
    if (h >= VFS_MAX_HANDLES || !handles[h].active) return VFS_ERR_INVAL;
    if (blk_block_size == 0)                        return VFS_ERR_IO;

    uint32_t to_write = length < (uint32_t)VFS_SHMEM_DATA_MAX
                      ? length : (uint32_t)VFS_SHMEM_DATA_MAX;

    /* Copy caller data into DMA region */
    const uint8_t *src = SHMEM_DATA();
    uint8_t       *dma = (uint8_t *)blk_dma_shmem_vaddr;
    for (uint32_t i = 0; i < to_write; i++) dma[i] = src[i];

    uint64_t first_block = offset / blk_block_size;
    uint32_t blk_count   = (to_write + blk_block_size - 1) / blk_block_size;

    uint32_t rc = blk_call(OP_BLK_WRITE, first_block, blk_count);
    if (rc != VFS_OK) {
        rep_u32(rep, 0, rc);
        rep_u32(rep, 4, 0);
        return rc;
    }

    rep_u32(rep, 0, VFS_OK);
    rep_u32(rep, 4, to_write);
    return VFS_OK;
}

/* blk_stat — return device-level size; mode = block device (0x6000) */
static uint32_t blk_stat(uint32_t h) {
    IPC_STUB_LOCALS
    if (h >= VFS_MAX_HANDLES || !handles[h].active) return VFS_ERR_INVAL;
    uint64_t total_bytes = blk_block_count * blk_block_size;
    rep_u32(rep, 0, VFS_OK);
    rep_u32(rep, 4, (uintptr_t)(total_bytes & 0xFFFFFFFFu));
    rep_u32(rep, 8, (uintptr_t)(total_bytes >> 32));
    rep_u32(rep, 12, 0x6000u);  /* block device */
    return VFS_OK;
}

/* Remaining blk ops return VFS_ERR_IO when blk absent, VFS_OK otherwise */
static uint32_t blk_unlink(const char *path) {
    IPC_STUB_LOCALS
    (void)path;
    if (!blk_present) { rep_u32(rep, 0, VFS_ERR_IO); return VFS_ERR_IO; }
    rep_u32(rep, 0, VFS_ERR_PERM);  /* raw block device — no unlink */
    return VFS_ERR_PERM;
}

static uint32_t blk_mkdir(const char *path) {
    IPC_STUB_LOCALS
    (void)path;
    if (!blk_present) { rep_u32(rep, 0, VFS_ERR_IO); return VFS_ERR_IO; }
    rep_u32(rep, 0, VFS_ERR_PERM);
    return VFS_ERR_PERM;
}

static uint32_t blk_readdir(const char *path) {
    IPC_STUB_LOCALS
    (void)path;
    if (!blk_present) { rep_u32(rep, 0, VFS_ERR_IO); return VFS_ERR_IO; }
    /* Emit a single pseudo-entry "." for the block device root */
    char *buf = SHMEM_READDIR();
    buf[0] = '.'; buf[1] = '\0';
    rep_u32(rep, 0, VFS_OK);
    rep_u32(rep, 4, 1);
    return VFS_OK;
}

static uint32_t blk_truncate(uint32_t h, uint64_t new_size) {
    IPC_STUB_LOCALS
    (void)new_size;
    if (h >= VFS_MAX_HANDLES || !handles[h].active) return VFS_ERR_INVAL;
    if (!blk_present) { rep_u32(rep, 0, VFS_ERR_IO); return VFS_ERR_IO; }
    rep_u32(rep, 0, VFS_ERR_PERM);
    return VFS_ERR_PERM;
}

static uint32_t blk_sync(uint32_t h) {
    IPC_STUB_LOCALS
    if (h >= VFS_MAX_HANDLES || !handles[h].active) return VFS_ERR_INVAL;
    if (!blk_present) { rep_u32(rep, 0, VFS_ERR_IO); return VFS_ERR_IO; }
    uint32_t rc = blk_call(OP_BLK_FLUSH, 0, 0);
    rep_u32(rep, 0, rc);
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Mount table management
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t vfs_mount(const char *prefix, uint8_t backend_type) {
    IPC_STUB_LOCALS
    /* Check for existing mount at same prefix */
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].active && vfs_strcmp(mounts[i].prefix, prefix) == 0)
            return VFS_ERR_EXISTS;
    }
    /* Find free slot */
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].active) {
            mounts[i].active       = true;
            mounts[i].backend_type = backend_type;
            vfs_strncpy(mounts[i].prefix, prefix, VFS_MEM_NAME_MAX);
            log_drain_write(VFS_CONSOLE_SLOT, VFS_PD_ID, "[vfs] mount ok\n");
            rep_u32(rep, 0, VFS_OK);
            return VFS_OK;
        }
    }
    rep_u32(rep, 0, VFS_ERR_NO_SPACE);
    return VFS_ERR_NO_SPACE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Microkit entry points
 * ═══════════════════════════════════════════════════════════════════════════ */

static void vfs_server_pd_init(void) {
    IPC_STUB_LOCALS
    agentos_log_boot("vfs_server");
    log_drain_write(VFS_CONSOLE_SLOT, VFS_PD_ID, "[vfs_server] init\n");

    /* ── Clear handle table ── */
    for (uint32_t i = 0; i < VFS_MAX_HANDLES; i++) {
        handles[i].active = false;
        handles[i].inode  = 0;
        handles[i].flags  = 0;
        handles[i].offset = 0;
    }
    open_handle_count = 0;

    /* ── Clear mount table ── */
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++)
        mounts[i].active = false;

    /* ── Clear inode table ── */
    for (uint32_t i = 0; i < VFS_MEM_MAX_INODES; i++) {
        mem_inodes[i].active     = false;
        mem_inodes[i].is_dir     = false;
        mem_inodes[i].size       = 0;
        mem_inodes[i].parent_ino = 0;
        mem_inodes[i].name[0]    = '\0';
    }

    /* ── Bootstrap root directory (inode 0) ── */
    mem_inodes[0].active     = true;
    mem_inodes[0].is_dir     = true;
    mem_inodes[0].parent_ino = 0;   /* root is its own parent */
    mem_inodes[0].size       = 0;
    mem_inodes[0].name[0]    = '/';
    mem_inodes[0].name[1]    = '\0';

    /* ── Default mount: "/" on MEM ── */
    mounts[0].active       = true;
    mounts[0].backend_type = VFS_BACKEND_MEM;
    mounts[0].prefix[0]    = '/';
    mounts[0].prefix[1]    = '\0';

    /* ── Probe virtio-blk via OP_BLK_INFO ── */
    rep_u32(rep, 0, OP_BLK_INFO);
    rep_u32(rep, 4, 0);
    rep_u32(rep, 8, 0);
    rep_u32(rep, 12, 0);
    /* E5-S8: ppcall stubbed */
    uint32_t blk_rc = (uint32_t)msg_u32(req, 0);

    if (blk_rc == BLK_OK) {
        uint64_t bc_lo  = (uint64_t)msg_u32(req, 4);
        uint64_t bc_hi  = (uint64_t)msg_u32(req, 8);
        blk_block_count = (bc_hi << 32) | bc_lo;
        blk_block_size  = (uint32_t)msg_u32(req, 12);
        blk_present     = true;

        /* Mount "/blk" on BLK backend */
        mounts[1].active       = true;
        mounts[1].backend_type = VFS_BACKEND_BLK;
        mounts[1].prefix[0]    = '/';
        mounts[1].prefix[1]    = 'b';
        mounts[1].prefix[2]    = 'l';
        mounts[1].prefix[3]    = 'k';
        mounts[1].prefix[4]    = '\0';

        /* Create "/blk" directory in the mem inode table so paths resolve */
        mem_inodes[1].active     = true;
        mem_inodes[1].is_dir     = true;
        mem_inodes[1].parent_ino = 0;
        mem_inodes[1].size       = 0;
        mem_inodes[1].name[0]    = 'b';
        mem_inodes[1].name[1]    = 'l';
        mem_inodes[1].name[2]    = 'k';
        mem_inodes[1].name[3]    = '\0';
        mem_dir_add_child(0, "blk");

        log_drain_write(VFS_CONSOLE_SLOT, VFS_PD_ID, "[vfs_server] virtio-blk present, /blk mounted\n");
    } else {
        blk_present = false;
        log_drain_write(VFS_CONSOLE_SLOT, VFS_PD_ID, "[vfs_server] virtio-blk absent, /blk stubbed\n");
    }

    log_drain_write(VFS_CONSOLE_SLOT, VFS_PD_ID, "[vfs_server] ALIVE\n");
}

/*
 * notified() — VFS Server is passive; we do not expect notifications.
 * Log anything unexpected so it surfaces in the console.
 */
static void vfs_server_pd_notified(uint32_t ch) {
    (void)ch;
    log_drain_write(VFS_CONSOLE_SLOT, VFS_PD_ID, "[vfs_server] unexpected notify\n");
}

/*
 * protected() — main VFS dispatcher.
 * All opcodes are dispatched by the label field (same as MR0 per convention
 * in this codebase — label carries the opcode for VFS callers).
 */
static uint32_t vfs_server_h_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;

    uint64_t op = msg_u32(req, 0);

    /* Path is always in shmem[48]; null-terminate defensively */
    char *path = SHMEM_PATH();
    path[VFS_SHMEM_PATH_MAX - 1] = '\0';

    switch (op) {

    /* ── OP_VFS_OPEN ────────────────────────────────────────────────────── */
    case OP_VFS_OPEN: {
        uint32_t flags   = (uint32_t)msg_u32(req, 4);
        uint8_t  backend = path_to_backend(path);

        uint32_t rc;
        if (backend == VFS_BACKEND_BLK)
            rc = blk_open(path, flags);
        else
            rc = mem_open(path, flags);

        if (rc != VFS_OK) {
            rep_u32(rep, 0, rc);
            rep_u32(rep, 4, 0);
            log_drain_write(VFS_CONSOLE_SLOT, VFS_PD_ID, "[vfs] open error\n");
        } else {
            log_drain_write(VFS_CONSOLE_SLOT, VFS_PD_ID, "[vfs] open ok\n");
        }
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    /* ── OP_VFS_CLOSE ───────────────────────────────────────────────────── */
    case OP_VFS_CLOSE: {
        uint32_t h  = (uint32_t)msg_u32(req, 4);
        uint32_t rc;
        if (h < VFS_MAX_HANDLES && handles[h].active &&
            handles[h].backend == VFS_BACKEND_BLK)
            rc = blk_close(h);
        else
            rc = mem_close(h);

        if (rc != VFS_OK) {
            rep_u32(rep, 0, rc);
            log_drain_write(VFS_CONSOLE_SLOT, VFS_PD_ID, "[vfs] close error\n");
        } else {
            log_drain_write(VFS_CONSOLE_SLOT, VFS_PD_ID, "[vfs] close ok\n");
        }
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    /* ── OP_VFS_READ ────────────────────────────────────────────────────── */
    case OP_VFS_READ: {
        uint32_t h      = (uint32_t)msg_u32(req, 4);
        uint64_t off_lo = (uint64_t)msg_u32(req, 8);
        uint64_t off_hi = (uint64_t)msg_u32(req, 12);
        uint32_t len    = (uint32_t)msg_u32(req, 16);
        uint64_t offset = (off_hi << 32) | off_lo;

        uint32_t rc;
        if (h < VFS_MAX_HANDLES && handles[h].active &&
            handles[h].backend == VFS_BACKEND_BLK)
            rc = blk_read(h, offset, len);
        else
            rc = mem_read(h, offset, len);

        if (rc != VFS_OK) {
            rep_u32(rep, 0, rc);
            rep_u32(rep, 4, 0);
        }
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    /* ── OP_VFS_WRITE ───────────────────────────────────────────────────── */
    case OP_VFS_WRITE: {
        uint32_t h      = (uint32_t)msg_u32(req, 4);
        uint64_t off_lo = (uint64_t)msg_u32(req, 8);
        uint64_t off_hi = (uint64_t)msg_u32(req, 12);
        uint32_t len    = (uint32_t)msg_u32(req, 16);
        uint64_t offset = (off_hi << 32) | off_lo;

        uint32_t rc;
        if (h < VFS_MAX_HANDLES && handles[h].active &&
            handles[h].backend == VFS_BACKEND_BLK)
            rc = blk_write(h, offset, len);
        else
            rc = mem_write(h, offset, len);

        if (rc != VFS_OK) {
            rep_u32(rep, 0, rc);
            rep_u32(rep, 4, 0);
        }
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    /* ── OP_VFS_STAT ────────────────────────────────────────────────────── */
    case OP_VFS_STAT: {
        uint32_t h  = (uint32_t)msg_u32(req, 4);
        uint32_t rc;
        if (h < VFS_MAX_HANDLES && handles[h].active &&
            handles[h].backend == VFS_BACKEND_BLK)
            rc = blk_stat(h);
        else
            rc = mem_stat(h);

        if (rc != VFS_OK) {
            rep_u32(rep, 0, rc);
            rep_u32(rep, 4, 0);
            rep_u32(rep, 8, 0);
            rep_u32(rep, 12, 0);
        }
        rep->length = 16;
        return SEL4_ERR_OK;
    }

    /* ── OP_VFS_UNLINK ──────────────────────────────────────────────────── */
    case OP_VFS_UNLINK: {
        uint8_t  backend = path_to_backend(path);
        uint32_t rc;
        if (backend == VFS_BACKEND_BLK)
            rc = blk_unlink(path);
        else
            rc = mem_unlink(path);

        if (rc != VFS_OK) {
            rep_u32(rep, 0, rc);
            log_drain_write(VFS_CONSOLE_SLOT, VFS_PD_ID, "[vfs] unlink error\n");
        }
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    /* ── OP_VFS_MKDIR ───────────────────────────────────────────────────── */
    case OP_VFS_MKDIR: {
        uint8_t  backend = path_to_backend(path);
        uint32_t rc;
        if (backend == VFS_BACKEND_BLK)
            rc = blk_mkdir(path);
        else
            rc = mem_mkdir(path);

        if (rc != VFS_OK) {
            rep_u32(rep, 0, rc);
            log_drain_write(VFS_CONSOLE_SLOT, VFS_PD_ID, "[vfs] mkdir error\n");
        }
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    /* ── OP_VFS_READDIR ─────────────────────────────────────────────────── */
    case OP_VFS_READDIR: {
        uint8_t  backend = path_to_backend(path);
        uint32_t rc;
        if (backend == VFS_BACKEND_BLK)
            rc = blk_readdir(path);
        else
            rc = mem_readdir(path);

        if (rc != VFS_OK) {
            rep_u32(rep, 0, rc);
            rep_u32(rep, 4, 0);
        }
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    /* ── OP_VFS_TRUNCATE ────────────────────────────────────────────────── */
    case OP_VFS_TRUNCATE: {
        uint32_t h       = (uint32_t)msg_u32(req, 4);
        uint64_t sz_lo   = (uint64_t)msg_u32(req, 8);
        uint64_t sz_hi   = (uint64_t)msg_u32(req, 12);
        uint64_t new_sz  = (sz_hi << 32) | sz_lo;

        uint32_t rc;
        if (h < VFS_MAX_HANDLES && handles[h].active &&
            handles[h].backend == VFS_BACKEND_BLK)
            rc = blk_truncate(h, new_sz);
        else
            rc = mem_truncate(h, new_sz);

        if (rc != VFS_OK) rep_u32(rep, 0, rc);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    /* ── OP_VFS_SYNC ────────────────────────────────────────────────────── */
    case OP_VFS_SYNC: {
        uint32_t h  = (uint32_t)msg_u32(req, 4);
        uint32_t rc;
        if (h < VFS_MAX_HANDLES && handles[h].active &&
            handles[h].backend == VFS_BACKEND_BLK)
            rc = blk_sync(h);
        else
            rc = mem_sync(h);

        if (rc != VFS_OK) rep_u32(rep, 0, rc);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    /* ── OP_VFS_MOUNT ───────────────────────────────────────────────────── */
    case OP_VFS_MOUNT: {
        uint32_t backend_type = (uint32_t)msg_u32(req, 4);
        /* prefix is in path buffer (shmem[48]) */
        uint32_t rc = vfs_mount(path, (uint8_t)backend_type);
        if (rc != VFS_OK) rep_u32(rep, 0, rc);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    /* ── OP_VFS_HEALTH ──────────────────────────────────────────────────── */
    case OP_VFS_HEALTH: {
        rep_u32(rep, 0, VFS_OK);
        rep_u32(rep, 4, open_handle_count);
        rep_u32(rep, 8, VFS_VERSION);
        rep->length = 12;
        return SEL4_ERR_OK;
    }

    /* ── Unknown opcode ─────────────────────────────────────────────────── */
    default:
        log_drain_write(VFS_CONSOLE_SLOT, VFS_PD_ID, "[vfs_server] unknown op\n");
        rep_u32(rep, 0, VFS_ERR_INVAL);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
}

/* ── E5-S8: Entry point ─────────────────────────────────────────────────── */
void vfs_server_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    vfs_server_pd_init();
    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    /* Dispatch all opcodes through the generic handler */
    sel4_server_register(&srv, SEL4_SERVER_OPCODE_ANY, vfs_server_h_dispatch, (void *)0);
    sel4_server_run(&srv);
}

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep) { vfs_server_main(my_ep, ns_ep); }
