/*
 * agentOS SpawnServer Protection Domain — E5-S5: raw seL4 IPC
 *
 * SpawnServer dynamically launches application PDs by assigning pre-allocated
 * "app slots" (app_slot_0..3) and loading ELF images into them via shared
 * memory.  The lifecycle is:
 *
 *   1. Caller writes app name + ELF path into spawn_config_shmem, then calls
 *      OP_SPAWN_LAUNCH (with cap_classes in data[0..3]).
 *   2. SpawnServer finds a free slot, opens the ELF from VFS, streams it into
 *      spawn_elf_shmem+SPAWN_HEADER_SIZE, writes a spawn_header_t at byte 0,
 *      then signals the slot endpoint directly.
 *   3. The app slot wakes up, loads the image, and replies with OP_SPAWN_LAUNCH
 *      to confirm it is "running".
 *   4. SpawnServer marks the slot state RUNNING and the app is live.
 *
 * Kill:  state → KILLED; slot endpoint signalled (slot resets itself).
 * Status/List: read-only queries answered directly from the slot table.
 *
 * Entry point:
 *   void spawn_server_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
 *
 * Outbound connections:
 *   "vfs_server" — looked up via nameserver
 *   app_slot_0..3 endpoints — received directly at boot as init_ep caps from
 *                              the root task's pd_init_ep table (NOT via nameserver)
 *
 * Priority: 120 (ACTIVE — runs independently, not purely passive).
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* ── Conditional compilation ─────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST
/*
 * Host-side test build: minimal stubs.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef unsigned long      seL4_CPtr;
typedef unsigned long long sel4_badge_t;

typedef struct {
    uint32_t opcode;
    uint32_t length;
    uint8_t  data[48];
} sel4_msg_t;

#define SEL4_ERR_OK          0u
#define SEL4_ERR_INVALID_OP  1u
#define SEL4_ERR_NOT_FOUND   2u
#define SEL4_ERR_BAD_ARG     4u
#define SEL4_ERR_NO_MEM      5u

typedef uint32_t (*sel4_handler_fn)(sel4_badge_t badge,
                                     const sel4_msg_t *req,
                                     sel4_msg_t *rep,
                                     void *ctx);
#define SEL4_SERVER_MAX_HANDLERS 32u
typedef struct {
    struct {
        uint32_t        opcode;
        sel4_handler_fn fn;
        void           *ctx;
    } handlers[SEL4_SERVER_MAX_HANDLERS];
    uint32_t  handler_count;
    seL4_CPtr ep;
} sel4_server_t;

static inline void sel4_server_init(sel4_server_t *srv, seL4_CPtr ep)
{
    srv->handler_count = 0;
    srv->ep            = ep;
    for (uint32_t i = 0; i < SEL4_SERVER_MAX_HANDLERS; i++) {
        srv->handlers[i].opcode = 0;
        srv->handlers[i].fn     = (sel4_handler_fn)0;
        srv->handlers[i].ctx    = (void *)0;
    }
}
static inline int sel4_server_register(sel4_server_t *srv, uint32_t opcode,
                                        sel4_handler_fn fn, void *ctx)
{
    if (srv->handler_count >= SEL4_SERVER_MAX_HANDLERS) return -1;
    srv->handlers[srv->handler_count].opcode = opcode;
    srv->handlers[srv->handler_count].fn     = fn;
    srv->handlers[srv->handler_count].ctx    = ctx;
    srv->handler_count++;
    return 0;
}
static inline uint32_t sel4_server_dispatch(sel4_server_t *srv,
                                             sel4_badge_t badge,
                                             const sel4_msg_t *req,
                                             sel4_msg_t *rep)
{
    for (uint32_t i = 0; i < srv->handler_count; i++) {
        if (srv->handlers[i].opcode == req->opcode) {
            uint32_t rc = srv->handlers[i].fn(badge, req, rep,
                                               srv->handlers[i].ctx);
            rep->opcode = rc;
            return rc;
        }
    }
    rep->opcode = SEL4_ERR_INVALID_OP;
    rep->length = 0;
    return SEL4_ERR_INVALID_OP;
}

/* seL4_DebugPutChar stub */
static inline void seL4_DebugPutChar(char c) { (void)c; }

/* sel4_call stub */
static sel4_msg_t _test_last_call_rep;
static inline void sel4_call(seL4_CPtr ep, const sel4_msg_t *req, sel4_msg_t *rep)
{
    (void)ep; (void)req;
    *rep = _test_last_call_rep;
}

#else /* !AGENTOS_TEST_HOST — production build */

#include <stdint.h>
#include <stdbool.h>
#include "sel4_ipc.h"     /* sel4_msg_t, sel4_badge_t, SEL4_ERR_* */
#include "sel4_server.h"  /* sel4_server_t, sel4_server_init/register/run */
#include "sel4_client.h"  /* sel4_client_connect, sel4_client_call */
#include <sel4/sel4.h>    /* seL4_DebugPutChar */

#endif /* AGENTOS_TEST_HOST */

/* ── Spawn contract constants ────────────────────────────────────────────── */

#ifndef SPAWN_VERSION
#define SPAWN_VERSION      1u
#endif
#ifndef SPAWN_MAX_SLOTS
#define SPAWN_MAX_SLOTS    4
#endif
#ifndef OP_SPAWN_LAUNCH
#define OP_SPAWN_LAUNCH    0xA0u
#endif
#ifndef OP_SPAWN_KILL
#define OP_SPAWN_KILL      0xA1u
#endif
#ifndef OP_SPAWN_STATUS
#define OP_SPAWN_STATUS    0xA2u
#endif
#ifndef OP_SPAWN_LIST
#define OP_SPAWN_LIST      0xA3u
#endif
#ifndef OP_SPAWN_HEALTH
#define OP_SPAWN_HEALTH    0xA4u
#endif
#ifndef SPAWN_OK
#define SPAWN_OK           0u
#endif
#ifndef SPAWN_ERR_NO_SLOTS
#define SPAWN_ERR_NO_SLOTS 1u
#endif
#ifndef SPAWN_ERR_NOT_FOUND
#define SPAWN_ERR_NOT_FOUND 2u
#endif
#ifndef SPAWN_ERR_VFS
#define SPAWN_ERR_VFS      3u
#endif
#ifndef SPAWN_ERR_TOO_LARGE
#define SPAWN_ERR_TOO_LARGE 4u
#endif
#ifndef SPAWN_ERR_INVAL
#define SPAWN_ERR_INVAL    5u
#endif
#ifndef SPAWN_SLOT_FREE
#define SPAWN_SLOT_FREE    0u
#endif
#ifndef SPAWN_SLOT_LOADING
#define SPAWN_SLOT_LOADING 1u
#endif
#ifndef SPAWN_SLOT_RUNNING
#define SPAWN_SLOT_RUNNING 2u
#endif
#ifndef SPAWN_SLOT_KILLED
#define SPAWN_SLOT_KILLED  3u
#endif
#ifndef SPAWN_MAGIC
#define SPAWN_MAGIC        0x5350574Eu  /* "SPWN" */
#endif
#ifndef SPAWN_HEADER_SIZE
#define SPAWN_HEADER_SIZE  96u
#endif
#ifndef SPAWN_MAX_ELF_SIZE
#define SPAWN_MAX_ELF_SIZE 0x7FF40u    /* 512KB - 96 bytes */
#endif
#ifndef SPAWN_CONFIG_NAME_MAX
#define SPAWN_CONFIG_NAME_MAX 32u
#endif
#ifndef SPAWN_CONFIG_PATH_MAX
#define SPAWN_CONFIG_PATH_MAX 128u
#endif
#ifndef SPAWN_CONFIG_NAME_OFF
#define SPAWN_CONFIG_NAME_OFF 0u
#endif
#ifndef SPAWN_CONFIG_PATH_OFF
#define SPAWN_CONFIG_PATH_OFF 32u
#endif
#ifndef SPAWN_CONFIG_LIST_OFF
#define SPAWN_CONFIG_LIST_OFF 160u
#endif

/* VFS opcodes */
#ifndef OP_VFS_OPEN
#define OP_VFS_OPEN   0xF0u
#endif
#ifndef OP_VFS_STAT
#define OP_VFS_STAT   0xF1u
#endif
#ifndef OP_VFS_READ
#define OP_VFS_READ   0xF2u
#endif
#ifndef OP_VFS_CLOSE
#define OP_VFS_CLOSE  0xF3u
#endif
#ifndef VFS_OK
#define VFS_OK        0u
#endif
#ifndef VFS_O_RD
#define VFS_O_RD      1u
#endif
#ifndef VFS_SHMEM_PATH_OFF
#define VFS_SHMEM_PATH_OFF  0u
#endif
#ifndef VFS_SHMEM_PATH_MAX
#define VFS_SHMEM_PATH_MAX  256u
#endif
#ifndef VFS_SHMEM_DATA_OFF
#define VFS_SHMEM_DATA_OFF  816u
#endif
#ifndef VFS_SHMEM_DATA_MAX
#define VFS_SHMEM_DATA_MAX  4096u
#endif

#ifndef OP_NS_REGISTER
#define OP_NS_REGISTER 0xD0u
#endif
#ifndef NS_NAME_MAX
#define NS_NAME_MAX    32
#endif
#ifndef TRACE_PD_SPAWN_SERVER
#define TRACE_PD_SPAWN_SERVER 29u
#endif

/* ── Shared memory base addresses ───────────────────────────────────────── */

/*
 * In production, root task sets these before calling spawn_server_main().
 * In test builds, the test harness sets them directly.
 */
uintptr_t spawn_elf_shmem_vaddr;
uintptr_t spawn_config_shmem_vaddr;
uintptr_t vfs_io_shmem_vaddr;

/* ── Slot table ──────────────────────────────────────────────────────────── */

typedef struct {
    bool      active;
    uint32_t  app_id;
    uint32_t  state;          /* SPAWN_SLOT_* */
    uint32_t  cap_classes;
    char      name[32];
    uint64_t  launch_tick;
    seL4_CPtr slot_ep;        /* direct cap to this app slot's endpoint */
} spawn_slot_t;

static spawn_slot_t slots[SPAWN_MAX_SLOTS];
static uint32_t     next_app_id   = 1;
static uint64_t     tick_counter  = 0;

/* Direct endpoint caps for app_slot_0..3, set by boot dispatcher */
static seL4_CPtr g_slot_eps[SPAWN_MAX_SLOTS];

/* Outbound service cap */
static seL4_CPtr g_vfs_ep = 0;

/* Server instance */
static sel4_server_t g_srv;

/* ── Data field helpers ───────────────────────────────────────────────────── */

static inline uint32_t data_rd32(const uint8_t *d, int off)
{
    return (uint32_t)d[off    ]
         | ((uint32_t)d[off+1] <<  8)
         | ((uint32_t)d[off+2] << 16)
         | ((uint32_t)d[off+3] << 24);
}

static inline void data_wr32(uint8_t *d, int off, uint32_t v)
{
    d[off  ] = (uint8_t)(v      );
    d[off+1] = (uint8_t)(v >>  8);
    d[off+2] = (uint8_t)(v >> 16);
    d[off+3] = (uint8_t)(v >> 24);
}

static inline void data_wr64(uint8_t *d, int off, uint64_t v)
{
    data_wr32(d, off,     (uint32_t)(v & 0xFFFFFFFFu));
    data_wr32(d, off + 4, (uint32_t)(v >> 32));
}

/* ── Debug output ────────────────────────────────────────────────────────── */

static void dbg_puts(const char *s)
{
#ifdef CONFIG_PRINTING
    for (; *s; s++)
        seL4_DebugPutChar(*s);
#else
    (void)s;
#endif
}

/* ── String helpers (no libc) ────────────────────────────────────────────── */

static void spawn_strlcpy(char *dst, const char *src, uint32_t n)
{
    uint32_t i = 0;
    for (; i < n - 1u && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static uint32_t spawn_strlen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

/* ── Slot helpers ─────────────────────────────────────────────────────────── */

static int find_free_slot(void)
{
    for (int i = 0; i < SPAWN_MAX_SLOTS; i++) {
        if (!slots[i].active) return i;
    }
    return -1;
}

static int find_slot_by_app_id(uint32_t app_id)
{
    for (int i = 0; i < SPAWN_MAX_SLOTS; i++) {
        if (slots[i].active && slots[i].app_id == app_id) return i;
    }
    return -1;
}

static uint32_t count_free_slots(void)
{
    uint32_t n = 0;
    for (int i = 0; i < SPAWN_MAX_SLOTS; i++) {
        if (!slots[i].active) n++;
    }
    return n;
}

/* ── VFS helpers ─────────────────────────────────────────────────────────── */

/*
 * vfs_open — write path into vfs shmem, call OP_VFS_OPEN, return handle.
 * Returns 0xFFFFFFFF on failure.
 */
static uint32_t vfs_open(const char *path)
{
    if (!g_vfs_ep || !vfs_io_shmem_vaddr) return 0xFFFFFFFFu;

    volatile uint8_t *path_buf =
        (volatile uint8_t *)(vfs_io_shmem_vaddr + VFS_SHMEM_PATH_OFF);
    uint32_t i = 0;
    for (; i < VFS_SHMEM_PATH_MAX - 1u && path[i]; i++)
        path_buf[i] = (uint8_t)path[i];
    path_buf[i] = 0;

    __asm__ volatile("" ::: "memory");

    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_VFS_OPEN;
    data_wr32(req.data, 0, VFS_O_RD);
    req.length = 4u;
    sel4_call(g_vfs_ep, &req, &rep);

    if (rep.opcode != VFS_OK) return 0xFFFFFFFFu;
    uint32_t handle = data_rd32(rep.data, 0);
    return (handle == 0u) ? 0xFFFFFFFFu : handle;
}

/*
 * vfs_stat — get file size for an open handle.
 * Returns 0xFFFFFFFF on failure or file too large.
 */
static uint32_t vfs_stat(uint32_t handle)
{
    if (!g_vfs_ep) return 0xFFFFFFFFu;
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_VFS_STAT;
    data_wr32(req.data, 0, handle);
    req.length = 4u;
    sel4_call(g_vfs_ep, &req, &rep);
    if (rep.opcode != VFS_OK) return 0xFFFFFFFFu;
    uint32_t size_hi = data_rd32(rep.data, 4);
    if (size_hi != 0u) return 0xFFFFFFFFu;
    return data_rd32(rep.data, 0);
}

/*
 * vfs_close — close an open handle.
 */
static void vfs_close(uint32_t handle)
{
    if (!g_vfs_ep) return;
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_VFS_CLOSE;
    data_wr32(req.data, 0, handle);
    req.length = 4u;
    sel4_call(g_vfs_ep, &req, &rep);
}

/*
 * vfs_read_all — read up to elf_size bytes from handle into dst via the VFS
 * shmem data area.  Returns total bytes read, or 0xFFFFFFFF on error.
 */
static uint32_t vfs_read_all(uint32_t handle, uint8_t *dst, uint32_t elf_size)
{
    if (!g_vfs_ep || !vfs_io_shmem_vaddr) return 0xFFFFFFFFu;

    const uint32_t chunk_max = VFS_SHMEM_DATA_MAX;
    uint32_t       offset    = 0;
    volatile uint8_t *data_buf =
        (volatile uint8_t *)(vfs_io_shmem_vaddr + VFS_SHMEM_DATA_OFF);

    while (offset < elf_size) {
        uint32_t remaining = elf_size - offset;
        uint32_t want      = remaining < chunk_max ? remaining : chunk_max;

        sel4_msg_t req = {0}, rep = {0};
        req.opcode = OP_VFS_READ;
        data_wr32(req.data, 0, handle);
        data_wr32(req.data, 4, offset);  /* offset_lo */
        data_wr32(req.data, 8, 0u);      /* offset_hi */
        data_wr32(req.data, 12, want);
        req.length = 16u;
        sel4_call(g_vfs_ep, &req, &rep);

        if (rep.opcode != VFS_OK) return 0xFFFFFFFFu;
        uint32_t bytes_read = data_rd32(rep.data, 0);
        if (bytes_read == 0u) return 0xFFFFFFFFu;

        __asm__ volatile("" ::: "memory");

        for (uint32_t i = 0; i < bytes_read; i++)
            dst[offset + i] = data_buf[i];

        offset += bytes_read;
        if (bytes_read < want) break; /* EOF */
    }
    return offset;
}

/* ── IPC handlers ────────────────────────────────────────────────────────── */

/*
 * handle_launch — OP_SPAWN_LAUNCH (0xA0)
 *
 * Request (data[]):
 *   data[0..3] = cap_classes  (uint32)
 *   data[4..7] = flags        (uint32, reserved)
 *   Name and ELF path are read from spawn_config_shmem.
 *
 * Reply:
 *   data[0..3]  = SPAWN_OK or SPAWN_ERR_*
 *   data[4..7]  = app_id  (on success)
 *   data[8..11] = slot_index (on success)
 */
static uint32_t handle_launch(sel4_badge_t badge, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    tick_counter++;

    uint32_t cap_classes = data_rd32(req->data, 0);

    if (!spawn_config_shmem_vaddr) {
        data_wr32(rep->data, 0, SPAWN_ERR_INVAL);
        rep->length = 4u;
        return SEL4_ERR_BAD_ARG;
    }

    /* Read name and elf_path from spawn_config_shmem */
    volatile uint8_t *cfg = (volatile uint8_t *)spawn_config_shmem_vaddr;

    char name[SPAWN_CONFIG_NAME_MAX];
    char elf_path[SPAWN_CONFIG_PATH_MAX];

    for (uint32_t i = 0; i < SPAWN_CONFIG_NAME_MAX; i++)
        name[i] = (char)cfg[SPAWN_CONFIG_NAME_OFF + i];
    name[SPAWN_CONFIG_NAME_MAX - 1u] = '\0';

    for (uint32_t i = 0; i < SPAWN_CONFIG_PATH_MAX; i++)
        elf_path[i] = (char)cfg[SPAWN_CONFIG_PATH_OFF + i];
    elf_path[SPAWN_CONFIG_PATH_MAX - 1u] = '\0';

    if (name[0] == '\0' || elf_path[0] == '\0') {
        dbg_puts("[spawn_server] LAUNCH REJECT: empty name or path\n");
        data_wr32(rep->data, 0, SPAWN_ERR_INVAL);
        rep->length = 4u;
        return SEL4_ERR_BAD_ARG;
    }

    int si = find_free_slot();
    if (si < 0) {
        dbg_puts("[spawn_server] LAUNCH REJECT: no free slots\n");
        data_wr32(rep->data, 0, SPAWN_ERR_NO_SLOTS);
        rep->length = 4u;
        return SEL4_ERR_NO_MEM;
    }

    dbg_puts("[spawn_server] LAUNCH: opening ELF via VFS\n");

    /* Open ELF via VFS */
    uint32_t vfs_handle = vfs_open(elf_path);
    if (vfs_handle == 0xFFFFFFFFu) {
        dbg_puts("[spawn_server] LAUNCH FAIL: VFS open failed\n");
        data_wr32(rep->data, 0, SPAWN_ERR_VFS);
        rep->length = 4u;
        return SEL4_ERR_INTERNAL;
    }

    uint32_t elf_size = vfs_stat(vfs_handle);
    if (elf_size == 0xFFFFFFFFu || elf_size == 0u ||
        elf_size > SPAWN_MAX_ELF_SIZE) {
        dbg_puts("[spawn_server] LAUNCH FAIL: VFS stat failed or size invalid\n");
        vfs_close(vfs_handle);
        data_wr32(rep->data, 0,
                  (elf_size == 0xFFFFFFFFu) ? SPAWN_ERR_VFS : SPAWN_ERR_TOO_LARGE);
        rep->length = 4u;
        return SEL4_ERR_INTERNAL;
    }

    uint8_t *elf_dst = (uint8_t *)(spawn_elf_shmem_vaddr + SPAWN_HEADER_SIZE);
    uint32_t bytes_read = vfs_read_all(vfs_handle, elf_dst, elf_size);
    vfs_close(vfs_handle);

    if (bytes_read != elf_size) {
        dbg_puts("[spawn_server] LAUNCH FAIL: VFS read incomplete\n");
        data_wr32(rep->data, 0, SPAWN_ERR_VFS);
        rep->length = 4u;
        return SEL4_ERR_INTERNAL;
    }

    uint32_t app_id = next_app_id++;
    tick_counter++;

    slots[si].active      = true;
    slots[si].app_id      = app_id;
    slots[si].state       = SPAWN_SLOT_LOADING;
    slots[si].cap_classes = cap_classes;
    slots[si].launch_tick = tick_counter;
    slots[si].slot_ep     = g_slot_eps[si];
    spawn_strlcpy(slots[si].name, name, 32u);

    /* Write spawn_header_t at spawn_elf_shmem[0..SPAWN_HEADER_SIZE-1] */
    if (spawn_elf_shmem_vaddr) {
        volatile uint32_t *hdr32 = (volatile uint32_t *)spawn_elf_shmem_vaddr;
        hdr32[0] = SPAWN_MAGIC;     /* magic */
        hdr32[1] = elf_size;        /* elf_size */
        hdr32[2] = cap_classes;     /* cap_classes */
        hdr32[3] = app_id;          /* app_id */
        hdr32[4] = 0u;              /* vfs_handle */
        hdr32[5] = 0u;              /* net_vnic_id */
        /* name[32] at byte offset 24 */
        volatile uint8_t *hdr_name =
            (volatile uint8_t *)spawn_elf_shmem_vaddr + 24u;
        for (uint32_t i = 0; i < 32u; i++)
            hdr_name[i] = (uint8_t)(i < 31u ? name[i] : 0);
        /* elf_sha256[32] at byte offset 56 (left as zero for MVP) */
    }

    __asm__ volatile("" ::: "memory");

    /* Signal the app slot */
    if (slots[si].slot_ep) {
        sel4_msg_t sig = {0}, sig_rep = {0};
        sig.opcode = OP_SPAWN_LAUNCH;
        data_wr32(sig.data, 0, app_id);
        data_wr32(sig.data, 4, cap_classes);
        sig.length = 8u;
        sel4_call(slots[si].slot_ep, &sig, &sig_rep);
        if (sig_rep.opcode == SEL4_ERR_OK)
            slots[si].state = SPAWN_SLOT_RUNNING;
    } else {
        /* No direct slot endpoint — mark running immediately (test/stub mode) */
        slots[si].state = SPAWN_SLOT_RUNNING;
    }

    dbg_puts("[spawn_server] LAUNCHED\n");

    data_wr32(rep->data, 0, SPAWN_OK);
    data_wr32(rep->data, 4, app_id);
    data_wr32(rep->data, 8, (uint32_t)si);
    rep->length = 12u;
    return SEL4_ERR_OK;
}

/*
 * handle_kill — OP_SPAWN_KILL (0xA1)
 *
 * Request:
 *   data[0..3] = app_id  (uint32)
 *
 * Reply:
 *   data[0..3] = SPAWN_OK or SPAWN_ERR_NOT_FOUND
 */
static uint32_t handle_kill(sel4_badge_t badge, const sel4_msg_t *req,
                             sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t app_id = data_rd32(req->data, 0);
    int si = find_slot_by_app_id(app_id);
    if (si < 0) {
        dbg_puts("[spawn_server] KILL: app_id not found\n");
        data_wr32(rep->data, 0, SPAWN_ERR_NOT_FOUND);
        rep->length = 4u;
        return SEL4_ERR_NOT_FOUND;
    }

    slots[si].state = SPAWN_SLOT_KILLED;

    /* Signal the slot to reset */
    if (slots[si].slot_ep) {
        sel4_msg_t sig = {0}, sig_rep = {0};
        sig.opcode = OP_SPAWN_KILL;
        data_wr32(sig.data, 0, app_id);
        sig.length = 4u;
        sel4_call(slots[si].slot_ep, &sig, &sig_rep);
    }

    slots[si].active = false;

    data_wr32(rep->data, 0, SPAWN_OK);
    rep->length = 4u;
    return SEL4_ERR_OK;
}

/*
 * handle_status — OP_SPAWN_STATUS (0xA2)
 *
 * Request:
 *   data[0..3] = app_id  (uint32; 0xFFFFFFFF = aggregate)
 *
 * Reply (aggregate):
 *   data[0..3] = SPAWN_OK
 *   data[4..7] = active_count
 *   data[8..11] = free_count
 *
 * Reply (specific app_id):
 *   data[0..3] = SPAWN_OK
 *   data[4..7] = state
 *   data[8..11] = cap_classes
 */
static uint32_t handle_status(sel4_badge_t badge, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t app_id = data_rd32(req->data, 0);

    if (app_id == 0xFFFFFFFFu) {
        uint32_t free_cnt = count_free_slots();
        data_wr32(rep->data, 0, SPAWN_OK);
        data_wr32(rep->data, 4, (uint32_t)SPAWN_MAX_SLOTS - free_cnt);
        data_wr32(rep->data, 8, free_cnt);
        rep->length = 12u;
        return SEL4_ERR_OK;
    }

    int si = find_slot_by_app_id(app_id);
    if (si < 0) {
        data_wr32(rep->data, 0, SPAWN_ERR_NOT_FOUND);
        rep->length = 4u;
        return SEL4_ERR_NOT_FOUND;
    }

    data_wr32(rep->data, 0, SPAWN_OK);
    data_wr32(rep->data, 4, slots[si].state);
    data_wr32(rep->data, 8, slots[si].cap_classes);
    rep->length = 12u;
    return SEL4_ERR_OK;
}

/*
 * handle_list — OP_SPAWN_LIST (0xA3)
 *
 * Writes slot info into spawn_config_shmem starting at SPAWN_CONFIG_LIST_OFF.
 *
 * Reply:
 *   data[0..3] = SPAWN_OK
 *   data[4..7] = count
 */
static uint32_t handle_list(sel4_badge_t badge, const sel4_msg_t *req,
                             sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;

    uint32_t count = 0;

    if (spawn_config_shmem_vaddr) {
        /* Write slot entries as packed structs (48 bytes each) */
        volatile uint8_t *list =
            (volatile uint8_t *)(spawn_config_shmem_vaddr + SPAWN_CONFIG_LIST_OFF);

        for (int i = 0; i < SPAWN_MAX_SLOTS; i++) {
            if (!slots[i].active) continue;
            volatile uint8_t *e = list + count * 48u;
            /* app_id */ e[0] = slots[i].app_id & 0xFF;
                          e[1] = (slots[i].app_id >> 8) & 0xFF;
                          e[2] = (slots[i].app_id >> 16) & 0xFF;
                          e[3] = (slots[i].app_id >> 24) & 0xFF;
            /* slot_index */ e[4] = (uint8_t)i;
                             e[5] = 0; e[6] = 0; e[7] = 0;
            /* state */       e[8]  = slots[i].state & 0xFF;
                              e[9] = 0; e[10] = 0; e[11] = 0;
            /* cap_classes */ e[12] = slots[i].cap_classes & 0xFF;
                              e[13] = (slots[i].cap_classes >> 8) & 0xFF;
                              e[14] = (slots[i].cap_classes >> 16) & 0xFF;
                              e[15] = (slots[i].cap_classes >> 24) & 0xFF;
            /* name[32] */
            for (int c = 0; c < 32; c++)
                e[16 + c] = (uint8_t)slots[i].name[c];
            count++;
        }
        __asm__ volatile("" ::: "memory");
    }

    data_wr32(rep->data, 0, SPAWN_OK);
    data_wr32(rep->data, 4, count);
    rep->length = 8u;
    return SEL4_ERR_OK;
}

/*
 * handle_health — OP_SPAWN_HEALTH (0xA4)
 *
 * Reply:
 *   data[0..3] = SPAWN_OK
 *   data[4..7] = free_slots
 *   data[8..11] = SPAWN_VERSION
 */
static uint32_t handle_health(sel4_badge_t badge, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;

    data_wr32(rep->data, 0, SPAWN_OK);
    data_wr32(rep->data, 4, count_free_slots());
    data_wr32(rep->data, 8, SPAWN_VERSION);
    rep->length = 12u;
    return SEL4_ERR_OK;
}

/* ── Nameserver registration ─────────────────────────────────────────────── */

static void register_with_nameserver(seL4_CPtr ns_ep)
{
    if (!ns_ep) return;
    sel4_msg_t req, rep;
    req.opcode = OP_NS_REGISTER;
    data_wr32(req.data, 0,  0u);
    data_wr32(req.data, 4,  TRACE_PD_SPAWN_SERVER);
    data_wr32(req.data, 8,  0u);
    data_wr32(req.data, 12, 1u);
    const char name[] = "spawn";
    for (int i = 0; i < NS_NAME_MAX; i++)
        req.data[16 + i] = (uint8_t)(i < (int)(sizeof(name) - 1) ? name[i] : 0);
    req.length = 48u;
    sel4_call(ns_ep, &req, &rep);
}

static seL4_CPtr lookup_service(seL4_CPtr ns_ep, const char *svc_name)
{
    if (!ns_ep) return 0;
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = 0xD1u; /* OP_NS_LOOKUP */
    for (int i = 0; i < NS_NAME_MAX; i++)
        req.data[i] = (uint8_t)(svc_name[i] ? svc_name[i] : 0);
    req.length = NS_NAME_MAX;
    sel4_call(ns_ep, &req, &rep);
    if (rep.opcode != 0u) return 0;
    return (seL4_CPtr)data_rd32(rep.data, 0);
}

/* ── Test-visible helpers ────────────────────────────────────────────────── */

static void spawn_server_test_init(void)
{
    for (int i = 0; i < SPAWN_MAX_SLOTS; i++) {
        slots[i].active      = false;
        slots[i].app_id      = 0;
        slots[i].state       = SPAWN_SLOT_FREE;
        slots[i].cap_classes = 0;
        slots[i].launch_tick = 0;
        slots[i].slot_ep     = 0;
        for (int c = 0; c < 32; c++) slots[i].name[c] = '\0';
        g_slot_eps[i] = 0;
    }
    next_app_id  = 1;
    tick_counter = 0;
    g_vfs_ep     = 0;
    spawn_elf_shmem_vaddr    = 0;
    spawn_config_shmem_vaddr = 0;
    vfs_io_shmem_vaddr       = 0;

    sel4_server_init(&g_srv, 0);
    sel4_server_register(&g_srv, OP_SPAWN_LAUNCH, handle_launch, NULL);
    sel4_server_register(&g_srv, OP_SPAWN_KILL,   handle_kill,   NULL);
    sel4_server_register(&g_srv, OP_SPAWN_STATUS, handle_status, NULL);
    sel4_server_register(&g_srv, OP_SPAWN_LIST,   handle_list,   NULL);
    sel4_server_register(&g_srv, OP_SPAWN_HEALTH, handle_health, NULL);
}

static uint32_t spawn_server_dispatch_one(sel4_badge_t badge,
                                           const sel4_msg_t *req,
                                           sel4_msg_t *rep)
{
    return sel4_server_dispatch(&g_srv, badge, req, rep);
}

static uint32_t spawn_server_get_free_slots(void) { return count_free_slots(); }

/* ── Entry point ─────────────────────────────────────────────────────────── */

#ifndef AGENTOS_TEST_HOST
void spawn_server_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    dbg_puts("[spawn_server] SpawnServer PD starting...\n");

    /* Initialise slot table */
    for (int i = 0; i < SPAWN_MAX_SLOTS; i++) {
        slots[i].active      = false;
        slots[i].app_id      = 0;
        slots[i].state       = SPAWN_SLOT_FREE;
        slots[i].cap_classes = 0;
        slots[i].launch_tick = 0;
        slots[i].slot_ep     = g_slot_eps[i];
        for (int c = 0; c < 32; c++) slots[i].name[c] = '\0';
    }
    next_app_id  = 1;
    tick_counter = 0;

    /* Register with nameserver as "spawn" */
    register_with_nameserver(ns_ep);

    /* Resolve VFS endpoint via nameserver */
    g_vfs_ep = lookup_service(ns_ep, "vfs_server");

    dbg_puts("[spawn_server] *** SpawnServer ALIVE — accepting launch requests ***\n");

    sel4_server_init(&g_srv, my_ep);
    sel4_server_register(&g_srv, OP_SPAWN_LAUNCH, handle_launch, NULL);
    sel4_server_register(&g_srv, OP_SPAWN_KILL,   handle_kill,   NULL);
    sel4_server_register(&g_srv, OP_SPAWN_STATUS, handle_status, NULL);
    sel4_server_register(&g_srv, OP_SPAWN_LIST,   handle_list,   NULL);
    sel4_server_register(&g_srv, OP_SPAWN_HEALTH, handle_health, NULL);

    /* Enter server loop — never returns */
    sel4_server_run(&g_srv);
}
#endif /* !AGENTOS_TEST_HOST */
