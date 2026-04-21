/*
 * test_memfs.c — API tests for the agentOS MemFS (object-store) service
 *
 * Covered opcodes:
 *   OP_MEMFS_OPEN    (0x30) — open or create a file, return a file descriptor
 *   OP_MEMFS_CLOSE   (0x31) — close an open file descriptor
 *   OP_MEMFS_READ    (0x32) — read bytes from the current seek position
 *   OP_MEMFS_WRITE   (0x33) — write bytes at the current seek position
 *   OP_MEMFS_SEEK    (0x34) — reposition the file pointer
 *   OP_MEMFS_STAT    (0x35) — query size and existence of a path
 *   OP_MEMFS_UNLINK  (0x36) — delete a file
 *   OP_MEMFS_MKDIR   (0x37) — create a directory entry
 *   unknown opcode           — must return AOS_ERR_UNIMPL
 *
 * The mock matches the ABI used by tests/vibe/mock_memfs.c and the
 * STORAGE_OP_* labels from that file.
 *
 * TODO: replace inline mock with
 *       #include "../../contracts/memfs/interface.h"
 *
 * Build & run:
 *   cc -DAGENTOS_TEST_HOST -I tests/api -o /tmp/t_memfs \
 *       tests/api/test_memfs.c && /tmp/t_memfs
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST
#include "framework.h"

/* ── Opcodes ─────────────────────────────────────────────────────────────── */
/* TODO: replace with #include "../../contracts/memfs/interface.h" */
#define OP_MEMFS_OPEN    0x30u
#define OP_MEMFS_CLOSE   0x31u
#define OP_MEMFS_READ    0x32u
#define OP_MEMFS_WRITE   0x33u
#define OP_MEMFS_SEEK    0x34u
#define OP_MEMFS_STAT    0x35u
#define OP_MEMFS_UNLINK  0x36u
#define OP_MEMFS_MKDIR   0x37u

/* ── SEEK whence values ───────────────────────────────────────────────────── */
#define MEMFS_SEEK_SET 0u
#define MEMFS_SEEK_CUR 1u
#define MEMFS_SEEK_END 2u

/* ── Mock MemFS ───────────────────────────────────────────────────────────── */

#define MOCK_MAX_FILES    32u
#define MOCK_MAX_FDS      16u
#define MOCK_MAX_PATH     128u
#define MOCK_MAX_DATA     4096u
#define MOCK_IS_DIR       0x80000000u

typedef struct {
    char     path[MOCK_MAX_PATH];
    uint8_t  data[MOCK_MAX_DATA];
    uint32_t size;
    uint32_t flags;     /* MOCK_IS_DIR for directories */
    uint32_t active;
} MockFile;

typedef struct {
    uint32_t file_idx;
    uint32_t pos;
    uint32_t active;
} MockFd;

static MockFile g_files[MOCK_MAX_FILES];
static MockFd   g_fds[MOCK_MAX_FDS];

static void memfs_reset(void) {
    memset(g_files, 0, sizeof(g_files));
    memset(g_fds,   0, sizeof(g_fds));
}

static int memfs_find_file(const char *path) {
    for (uint32_t i = 0; i < MOCK_MAX_FILES; i++) {
        if (g_files[i].active && strcmp(g_files[i].path, path) == 0)
            return (int)i;
    }
    return -1;
}

static int memfs_alloc_file(void) {
    for (uint32_t i = 0; i < MOCK_MAX_FILES; i++) {
        if (!g_files[i].active) return (int)i;
    }
    return -1;
}

static int memfs_alloc_fd(void) {
    for (uint32_t i = 0; i < MOCK_MAX_FDS; i++) {
        if (!g_fds[i].active) return (int)i;
    }
    return -1;
}

/*
 * memfs_dispatch — mock MemFS IPC handler.
 *
 * MR layout:
 *   MR[0]  = opcode
 *   MR[1+] = op-specific (see per-case comments)
 * Reply:
 *   MR[0]  = AOS_OK | AOS_ERR_*
 *   MR[1+] = op-specific results
 */
static void memfs_dispatch(microkit_channel ch, microkit_msginfo info) {
    (void)ch; (void)info;
    uint64_t op = _mrs[0];

    switch (op) {

    /* ── OPEN ───────────────────────────────────────────────────────────────
     * In:  MR[1] = path ptr, MR[2] = path len, MR[3] = flags (O_CREAT=1)
     * Out: MR[0] = AOS_OK | AOS_ERR_NOT_FOUND | AOS_ERR_NOSPC
     *      MR[1] = fd (on success, 1-based)
     */
    case OP_MEMFS_OPEN: {
        const char *path = (const char *)(uintptr_t)_mrs[1];
        uint32_t    plen = (uint32_t)_mrs[2];
        uint32_t    flags = (uint32_t)_mrs[3];
        if (!path || plen == 0 || plen >= MOCK_MAX_PATH) {
            _mrs[0] = AOS_ERR_INVAL; break;
        }
        char tmp[MOCK_MAX_PATH];
        memcpy(tmp, path, plen); tmp[plen] = '\0';

        int fidx = memfs_find_file(tmp);
        if (fidx < 0) {
            if (!(flags & 1u)) {   /* O_CREAT not set */
                _mrs[0] = AOS_ERR_NOT_FOUND; break;
            }
            fidx = memfs_alloc_file();
            if (fidx < 0) { _mrs[0] = AOS_ERR_NOSPC; break; }
            memcpy(g_files[fidx].path, tmp, plen + 1);
            g_files[fidx].size   = 0;
            g_files[fidx].flags  = 0;
            g_files[fidx].active = 1u;
        }
        int fdidx = memfs_alloc_fd();
        if (fdidx < 0) { _mrs[0] = AOS_ERR_NOSPC; break; }
        g_fds[fdidx].file_idx = (uint32_t)fidx;
        g_fds[fdidx].pos      = 0;
        g_fds[fdidx].active   = 1u;
        _mrs[0] = AOS_OK;
        _mrs[1] = (uint64_t)(fdidx + 1);  /* 1-based fd */
        break;
    }

    /* ── CLOSE ──────────────────────────────────────────────────────────────
     * In:  MR[1] = fd
     * Out: MR[0] = AOS_OK | AOS_ERR_NOT_FOUND
     */
    case OP_MEMFS_CLOSE: {
        uint32_t fd = (uint32_t)_mrs[1];
        if (fd == 0 || fd > MOCK_MAX_FDS || !g_fds[fd - 1].active) {
            _mrs[0] = AOS_ERR_NOT_FOUND; break;
        }
        g_fds[fd - 1].active = 0u;
        _mrs[0] = AOS_OK;
        break;
    }

    /* ── READ ───────────────────────────────────────────────────────────────
     * In:  MR[1] = fd, MR[2] = buf_ptr, MR[3] = count
     * Out: MR[0] = AOS_OK | AOS_ERR_NOT_FOUND
     *      MR[1] = bytes_read
     */
    case OP_MEMFS_READ: {
        uint32_t fd = (uint32_t)_mrs[1];
        uint8_t *buf = (uint8_t *)(uintptr_t)_mrs[2];
        uint32_t cnt = (uint32_t)_mrs[3];
        if (fd == 0 || fd > MOCK_MAX_FDS || !g_fds[fd - 1].active) {
            _mrs[0] = AOS_ERR_NOT_FOUND; break;
        }
        MockFd   *f    = &g_fds[fd - 1];
        MockFile *file = &g_files[f->file_idx];
        uint32_t avail = (f->pos < file->size) ? file->size - f->pos : 0;
        uint32_t n     = (cnt < avail) ? cnt : avail;
        if (buf && n > 0) memcpy(buf, file->data + f->pos, n);
        f->pos += n;
        _mrs[0] = AOS_OK;
        _mrs[1] = (uint64_t)n;
        break;
    }

    /* ── WRITE ──────────────────────────────────────────────────────────────
     * In:  MR[1] = fd, MR[2] = buf_ptr, MR[3] = count
     * Out: MR[0] = AOS_OK | AOS_ERR_NOT_FOUND | AOS_ERR_NOSPC
     *      MR[1] = bytes_written
     */
    case OP_MEMFS_WRITE: {
        uint32_t fd = (uint32_t)_mrs[1];
        const uint8_t *buf = (const uint8_t *)(uintptr_t)_mrs[2];
        uint32_t cnt = (uint32_t)_mrs[3];
        if (fd == 0 || fd > MOCK_MAX_FDS || !g_fds[fd - 1].active) {
            _mrs[0] = AOS_ERR_NOT_FOUND; break;
        }
        MockFd   *f    = &g_fds[fd - 1];
        MockFile *file = &g_files[f->file_idx];
        uint32_t end   = f->pos + cnt;
        if (end > MOCK_MAX_DATA) {
            _mrs[0] = AOS_ERR_NOSPC; break;
        }
        if (buf && cnt > 0) memcpy(file->data + f->pos, buf, cnt);
        f->pos += cnt;
        if (f->pos > file->size) file->size = f->pos;
        _mrs[0] = AOS_OK;
        _mrs[1] = (uint64_t)cnt;
        break;
    }

    /* ── SEEK ───────────────────────────────────────────────────────────────
     * In:  MR[1] = fd, MR[2] = offset (signed), MR[3] = whence
     * Out: MR[0] = AOS_OK | AOS_ERR_NOT_FOUND | AOS_ERR_INVAL
     *      MR[1] = new position
     */
    case OP_MEMFS_SEEK: {
        uint32_t fd     = (uint32_t)_mrs[1];
        int64_t  offset = (int64_t)_mrs[2];
        uint32_t whence = (uint32_t)_mrs[3];
        if (fd == 0 || fd > MOCK_MAX_FDS || !g_fds[fd - 1].active) {
            _mrs[0] = AOS_ERR_NOT_FOUND; break;
        }
        MockFd   *f    = &g_fds[fd - 1];
        MockFile *file = &g_files[f->file_idx];
        int64_t newpos;
        if      (whence == MEMFS_SEEK_SET) newpos = offset;
        else if (whence == MEMFS_SEEK_CUR) newpos = (int64_t)f->pos + offset;
        else if (whence == MEMFS_SEEK_END) newpos = (int64_t)file->size + offset;
        else { _mrs[0] = AOS_ERR_INVAL; break; }
        if (newpos < 0) { _mrs[0] = AOS_ERR_INVAL; break; }
        f->pos = (uint32_t)newpos;
        _mrs[0] = AOS_OK;
        _mrs[1] = (uint64_t)f->pos;
        break;
    }

    /* ── STAT ───────────────────────────────────────────────────────────────
     * In:  MR[1] = path ptr, MR[2] = path len
     * Out: MR[0] = AOS_OK
     *      MR[1] = size (0 if not found), MR[2] = exists (1/0)
     */
    case OP_MEMFS_STAT: {
        const char *path = (const char *)(uintptr_t)_mrs[1];
        uint32_t    plen = (uint32_t)_mrs[2];
        if (!path || plen == 0 || plen >= MOCK_MAX_PATH) {
            _mrs[0] = AOS_ERR_INVAL; break;
        }
        char tmp[MOCK_MAX_PATH];
        memcpy(tmp, path, plen); tmp[plen] = '\0';
        int fidx = memfs_find_file(tmp);
        _mrs[0] = AOS_OK;
        if (fidx < 0) {
            _mrs[1] = 0; _mrs[2] = 0;
        } else {
            _mrs[1] = (uint64_t)g_files[fidx].size;
            _mrs[2] = 1u;
        }
        break;
    }

    /* ── UNLINK ─────────────────────────────────────────────────────────────
     * In:  MR[1] = path ptr, MR[2] = path len
     * Out: MR[0] = AOS_OK | AOS_ERR_NOT_FOUND
     */
    case OP_MEMFS_UNLINK: {
        const char *path = (const char *)(uintptr_t)_mrs[1];
        uint32_t    plen = (uint32_t)_mrs[2];
        if (!path || plen == 0 || plen >= MOCK_MAX_PATH) {
            _mrs[0] = AOS_ERR_INVAL; break;
        }
        char tmp[MOCK_MAX_PATH];
        memcpy(tmp, path, plen); tmp[plen] = '\0';
        int fidx = memfs_find_file(tmp);
        if (fidx < 0) { _mrs[0] = AOS_ERR_NOT_FOUND; break; }
        memset(&g_files[fidx], 0, sizeof(g_files[fidx]));
        _mrs[0] = AOS_OK;
        break;
    }

    /* ── MKDIR ──────────────────────────────────────────────────────────────
     * In:  MR[1] = path ptr, MR[2] = path len
     * Out: MR[0] = AOS_OK | AOS_ERR_EXISTS | AOS_ERR_NOSPC
     */
    case OP_MEMFS_MKDIR: {
        const char *path = (const char *)(uintptr_t)_mrs[1];
        uint32_t    plen = (uint32_t)_mrs[2];
        if (!path || plen == 0 || plen >= MOCK_MAX_PATH) {
            _mrs[0] = AOS_ERR_INVAL; break;
        }
        char tmp[MOCK_MAX_PATH];
        memcpy(tmp, path, plen); tmp[plen] = '\0';
        if (memfs_find_file(tmp) >= 0) { _mrs[0] = AOS_ERR_EXISTS; break; }
        int fidx = memfs_alloc_file();
        if (fidx < 0) { _mrs[0] = AOS_ERR_NOSPC; break; }
        memcpy(g_files[fidx].path, tmp, plen + 1);
        g_files[fidx].size   = 0;
        g_files[fidx].flags  = MOCK_IS_DIR;
        g_files[fidx].active = 1u;
        _mrs[0] = AOS_OK;
        break;
    }

    default:
        _mrs[0] = AOS_ERR_UNIMPL;
        break;
    }
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static uint32_t do_open(const char *path, uint32_t flags) {
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_OPEN;
    _mrs[1] = (uint64_t)(uintptr_t)path;
    _mrs[2] = (uint64_t)strlen(path);
    _mrs[3] = (uint64_t)flags;
    memfs_dispatch(0, 0);
    return (_mrs[0] == AOS_OK) ? (uint32_t)_mrs[1] : 0u;
}

static uint32_t do_write(uint32_t fd, const char *data) {
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_WRITE;
    _mrs[1] = (uint64_t)fd;
    _mrs[2] = (uint64_t)(uintptr_t)data;
    _mrs[3] = (uint64_t)strlen(data);
    memfs_dispatch(0, 0);
    return (uint32_t)_mrs[1];
}

/* ════════════════════════════════════════════════════════════════════════════
 * Tests
 * ════════════════════════════════════════════════════════════════════════════ */

static void test_open_create_ok(void) {
    memfs_reset();
    uint32_t fd = do_open("/tmp/a.txt", 1u /* O_CREAT */);
    ASSERT_NE(fd, 0u, "OPEN: O_CREAT returns non-zero fd");
}

static void test_open_existing_ok(void) {
    memfs_reset();
    do_open("/tmp/b.txt", 1u);
    uint32_t fd = do_open("/tmp/b.txt", 0u);  /* open without O_CREAT */
    ASSERT_NE(fd, 0u, "OPEN: open existing file without O_CREAT succeeds");
}

static void test_open_not_found(void) {
    memfs_reset();
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_OPEN;
    _mrs[1] = (uint64_t)(uintptr_t)"/no/such/file";
    _mrs[2] = strlen("/no/such/file");
    _mrs[3] = 0;  /* no O_CREAT */
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND, "OPEN: missing file without O_CREAT returns AOS_ERR_NOT_FOUND");
}

static void test_open_null_path(void) {
    memfs_reset();
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_OPEN;
    _mrs[1] = 0;
    _mrs[2] = 5;
    _mrs[3] = 0;
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_INVAL, "OPEN: null path returns AOS_ERR_INVAL");
}

static void test_close_ok(void) {
    memfs_reset();
    uint32_t fd = do_open("/x", 1u);
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_CLOSE;
    _mrs[1] = (uint64_t)fd;
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "CLOSE: valid fd returns AOS_OK");
}

static void test_close_invalid_fd(void) {
    memfs_reset();
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_CLOSE;
    _mrs[1] = 9999u;
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND, "CLOSE: invalid fd returns AOS_ERR_NOT_FOUND");
}

static void test_close_prevents_read(void) {
    memfs_reset();
    uint32_t fd = do_open("/y", 1u);
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_CLOSE; _mrs[1] = fd;
    memfs_dispatch(0, 0);

    static char buf[32];
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_READ;
    _mrs[1] = (uint64_t)fd;
    _mrs[2] = (uint64_t)(uintptr_t)buf;
    _mrs[3] = sizeof(buf);
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND, "CLOSE: read on closed fd returns AOS_ERR_NOT_FOUND");
}

static void test_write_ok(void) {
    memfs_reset();
    uint32_t fd = do_open("/w.txt", 1u);
    uint32_t written = do_write(fd, "hello");
    ASSERT_EQ(written, 5u, "WRITE: 5 bytes reported written");
    ASSERT_EQ(_mrs[0], AOS_OK, "WRITE: status AOS_OK");
}

static void test_write_invalid_fd(void) {
    memfs_reset();
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_WRITE;
    _mrs[1] = 999u;
    _mrs[2] = (uint64_t)(uintptr_t)"x";
    _mrs[3] = 1;
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND, "WRITE: invalid fd returns AOS_ERR_NOT_FOUND");
}

static void test_write_overflow(void) {
    memfs_reset();
    uint32_t fd = do_open("/big.txt", 1u);
    static uint8_t huge[MOCK_MAX_DATA + 8];
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_WRITE;
    _mrs[1] = (uint64_t)fd;
    _mrs[2] = (uint64_t)(uintptr_t)huge;
    _mrs[3] = sizeof(huge);
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOSPC, "WRITE: overflow returns AOS_ERR_NOSPC");
}

static void test_read_ok(void) {
    memfs_reset();
    uint32_t fd = do_open("/r.txt", 1u);
    do_write(fd, "world");

    /* Rewind to 0 */
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_SEEK; _mrs[1] = fd;
    _mrs[2] = 0; _mrs[3] = MEMFS_SEEK_SET;
    memfs_dispatch(0, 0);

    static char buf[16];
    memset(buf, 0, sizeof(buf));
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_READ;
    _mrs[1] = (uint64_t)fd;
    _mrs[2] = (uint64_t)(uintptr_t)buf;
    _mrs[3] = 5;
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "READ: ok after seek-to-start");
    ASSERT_EQ(_mrs[1], 5u, "READ: 5 bytes returned");
    ASSERT_TRUE(memcmp(buf, "world", 5) == 0, "READ: content matches written data");
}

static void test_read_past_eof(void) {
    memfs_reset();
    uint32_t fd = do_open("/eof.txt", 1u);
    do_write(fd, "hi");

    /* Seek far past end */
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_SEEK; _mrs[1] = fd;
    _mrs[2] = (uint64_t)1000; _mrs[3] = MEMFS_SEEK_SET;
    memfs_dispatch(0, 0);

    static char buf[8];
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_READ;
    _mrs[1] = fd; _mrs[2] = (uint64_t)(uintptr_t)buf; _mrs[3] = 4;
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "READ: past-EOF returns AOS_OK");
    ASSERT_EQ(_mrs[1], 0u, "READ: 0 bytes when past EOF");
}

static void test_seek_set(void) {
    memfs_reset();
    uint32_t fd = do_open("/seek.txt", 1u);
    do_write(fd, "abcde");

    mock_mr_clear();
    _mrs[0] = OP_MEMFS_SEEK; _mrs[1] = fd;
    _mrs[2] = 2; _mrs[3] = MEMFS_SEEK_SET;
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "SEEK_SET: status AOS_OK");
    ASSERT_EQ(_mrs[1], 2u, "SEEK_SET: position == 2");
}

static void test_seek_cur(void) {
    memfs_reset();
    uint32_t fd = do_open("/seek2.txt", 1u);
    do_write(fd, "abcde");

    /* Position is now 5; move back by 3 using SEEK_CUR */
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_SEEK; _mrs[1] = fd;
    _mrs[2] = (uint64_t)(int64_t)-3; _mrs[3] = MEMFS_SEEK_CUR;
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "SEEK_CUR: status AOS_OK");
    ASSERT_EQ(_mrs[1], 2u, "SEEK_CUR: position == 2 after -3 from end");
}

static void test_seek_end(void) {
    memfs_reset();
    uint32_t fd = do_open("/seek3.txt", 1u);
    do_write(fd, "hello");

    mock_mr_clear();
    _mrs[0] = OP_MEMFS_SEEK; _mrs[1] = fd;
    _mrs[2] = 0; _mrs[3] = MEMFS_SEEK_END;
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "SEEK_END: status AOS_OK");
    ASSERT_EQ(_mrs[1], 5u, "SEEK_END: position == file size");
}

static void test_seek_invalid_whence(void) {
    memfs_reset();
    uint32_t fd = do_open("/s.txt", 1u);
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_SEEK; _mrs[1] = fd;
    _mrs[2] = 0; _mrs[3] = 0x99u;  /* bad whence */
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_INVAL, "SEEK: invalid whence returns AOS_ERR_INVAL");
}

static void test_seek_negative_result(void) {
    memfs_reset();
    uint32_t fd = do_open("/neg.txt", 1u);
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_SEEK; _mrs[1] = fd;
    _mrs[2] = (uint64_t)(int64_t)-10; _mrs[3] = MEMFS_SEEK_SET;
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_INVAL, "SEEK: negative result returns AOS_ERR_INVAL");
}

static void test_stat_existing(void) {
    memfs_reset();
    uint32_t fd = do_open("/stat.txt", 1u);
    do_write(fd, "abc");

    mock_mr_clear();
    _mrs[0] = OP_MEMFS_STAT;
    _mrs[1] = (uint64_t)(uintptr_t)"/stat.txt";
    _mrs[2] = strlen("/stat.txt");
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "STAT: existing file returns AOS_OK");
    ASSERT_EQ(_mrs[2], 1u, "STAT: exists == 1 for existing file");
    ASSERT_EQ(_mrs[1], 3u, "STAT: size == 3 after 3-byte write");
}

static void test_stat_missing(void) {
    memfs_reset();
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_STAT;
    _mrs[1] = (uint64_t)(uintptr_t)"/ghost";
    _mrs[2] = strlen("/ghost");
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "STAT: missing file returns AOS_OK");
    ASSERT_EQ(_mrs[2], 0u, "STAT: exists == 0 for missing file");
}

static void test_unlink_ok(void) {
    memfs_reset();
    do_open("/del.txt", 1u);
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_UNLINK;
    _mrs[1] = (uint64_t)(uintptr_t)"/del.txt";
    _mrs[2] = strlen("/del.txt");
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "UNLINK: existing file returns AOS_OK");

    /* Stat must show it gone */
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_STAT;
    _mrs[1] = (uint64_t)(uintptr_t)"/del.txt";
    _mrs[2] = strlen("/del.txt");
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[2], 0u, "UNLINK: stat after unlink shows not-found");
}

static void test_unlink_missing(void) {
    memfs_reset();
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_UNLINK;
    _mrs[1] = (uint64_t)(uintptr_t)"/nope";
    _mrs[2] = strlen("/nope");
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_NOT_FOUND, "UNLINK: missing file returns AOS_ERR_NOT_FOUND");
}

static void test_mkdir_ok(void) {
    memfs_reset();
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_MKDIR;
    _mrs[1] = (uint64_t)(uintptr_t)"/tmp";
    _mrs[2] = strlen("/tmp");
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_OK, "MKDIR: creates new directory");
}

static void test_mkdir_duplicate(void) {
    memfs_reset();
    mock_mr_clear();
    _mrs[0] = OP_MEMFS_MKDIR;
    _mrs[1] = (uint64_t)(uintptr_t)"/dup";
    _mrs[2] = strlen("/dup");
    memfs_dispatch(0, 0);

    mock_mr_clear();
    _mrs[0] = OP_MEMFS_MKDIR;
    _mrs[1] = (uint64_t)(uintptr_t)"/dup";
    _mrs[2] = strlen("/dup");
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_EXISTS, "MKDIR: duplicate path returns AOS_ERR_EXISTS");
}

static void test_unknown_opcode(void) {
    memfs_reset();
    mock_mr_clear();
    _mrs[0] = 0xEEu;
    memfs_dispatch(0, 0);
    ASSERT_EQ(_mrs[0], AOS_ERR_UNIMPL, "unknown opcode returns AOS_ERR_UNIMPL");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    TAP_PLAN(39);

    test_open_create_ok();
    test_open_existing_ok();
    test_open_not_found();
    test_open_null_path();

    test_close_ok();
    test_close_invalid_fd();
    test_close_prevents_read();

    test_write_ok();
    test_write_invalid_fd();
    test_write_overflow();

    test_read_ok();
    test_read_past_eof();

    test_seek_set();
    test_seek_cur();
    test_seek_end();
    test_seek_invalid_whence();
    test_seek_negative_result();

    test_stat_existing();
    test_stat_missing();

    test_unlink_ok();
    test_unlink_missing();

    test_mkdir_ok();
    test_mkdir_duplicate();

    test_unknown_opcode();

    /* Round-trip: create, write, seek, read, close, unlink */
    {
        memfs_reset();
        uint32_t fd = do_open("/rt.txt", 1u);
        do_write(fd, "roundtrip");
        mock_mr_clear();
        _mrs[0] = OP_MEMFS_SEEK; _mrs[1] = fd; _mrs[2] = 0; _mrs[3] = MEMFS_SEEK_SET;
        memfs_dispatch(0, 0);
        static char buf[16]; memset(buf, 0, sizeof(buf));
        mock_mr_clear();
        _mrs[0] = OP_MEMFS_READ; _mrs[1] = fd;
        _mrs[2] = (uint64_t)(uintptr_t)buf; _mrs[3] = 9;
        memfs_dispatch(0, 0);
        ASSERT_EQ(_mrs[1], 9u, "round-trip: 9 bytes read back");
        ASSERT_TRUE(memcmp(buf, "roundtrip", 9) == 0, "round-trip: content matches");

        mock_mr_clear(); _mrs[0] = OP_MEMFS_CLOSE; _mrs[1] = fd;
        memfs_dispatch(0, 0);
        mock_mr_clear(); _mrs[0] = OP_MEMFS_UNLINK;
        _mrs[1] = (uint64_t)(uintptr_t)"/rt.txt"; _mrs[2] = strlen("/rt.txt");
        memfs_dispatch(0, 0);
        ASSERT_EQ(_mrs[0], AOS_OK, "round-trip: unlink after close succeeds");
    }

    /* MKDIR then open inside it */
    {
        memfs_reset();
        mock_mr_clear();
        _mrs[0] = OP_MEMFS_MKDIR;
        _mrs[1] = (uint64_t)(uintptr_t)"/data";
        _mrs[2] = strlen("/data");
        memfs_dispatch(0, 0);
        uint32_t fd2 = do_open("/data/file.bin", 1u);
        ASSERT_NE(fd2, 0u, "MKDIR+OPEN: file inside new dir can be created");
    }

    return tap_exit();
}

#else
typedef int _agentos_api_test_memfs_dummy;
#endif /* AGENTOS_TEST_HOST */
