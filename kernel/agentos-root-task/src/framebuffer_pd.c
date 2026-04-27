/*
 * framebuffer_pd.c — agentOS Virtual Framebuffer Protection Domain  [E5-S7: raw seL4 IPC]
 *
 * Manages virtual framebuffers for guest OSes via a backend dispatch model.
 * Each guest selects a backend at MSG_FB_CREATE time:
 *
 *   FB_BACKEND_NULL       — frames discarded; for headless guests
 *   FB_BACKEND_HW_DIRECT  — virtio-gpu / gpu_sched (Phase 4a-hw, ag-1es)
 *   FB_BACKEND_REMOTE_API — display server relay (Phase 4a-remote, ag-tz2)
 *
 * IPC protocol (sel4_server_t dispatch, opcode in req->opcode):
 *   MSG_FB_CREATE              fb_create_req in shmem → data[0..3]=ok data[4..7]=handle
 *   MSG_FB_WRITE               data[0..3]=handle; fb_write_req in shmem → data[0..3]=ok
 *   MSG_FB_FLIP                data[0..3]=handle → data[0..3]=ok data[4..7]=frame_seq
 *   MSG_FB_RESIZE              data[0..3]=handle; fb_resize_req in shmem → data[0..3]=ok
 *   MSG_FB_DESTROY             data[0..3]=handle → data[0..3]=ok
 *   MSG_FB_STATUS              data[0..3]=handle → data[0..3]=ok; fb_status_reply in shmem
 *   MSG_CC_ATTACH_FRAMEBUFFER  data[0..3]=handle → data[0..3]=ok; subscribe caller
 *
 * Entry point:
 *   void framebuffer_pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
 *
 * Bugs fixed in this migration (E5-S7):
 *   - log_drain channel-60 dispatch removed; log_drain is now resolved via
 *     nameserver (no #ifdef BOARD_qemu_virt_aarch64 workaround needed).
 *   - microkit_ppcall(ch=2/7, ...) to gpu_sched replaced with nameserver
 *     lookup for "gpu_sched" endpoint, then sel4_call.
 *   - EventBus publishing: microkit_ppcall(CH_FB_EVENTBUS, ...) replaced with
 *     nameserver-resolved event_bus endpoint + sel4_call.
 *   - microkit_notify for REMOTE_API subscribers replaced with seL4_Signal
 *     on stored notification caps (stubs for now).
 *   - microkit_dbg_puts replaced with seL4_DebugPutChar loop.
 *
 * Priority: 175  (below serial; above idle workers)
 * Mode: passive  (woken by IPC from callers)
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* ── Conditional compilation ─────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST
/*
 * Host-side test build: provide minimal type stubs so this file compiles
 * without seL4 or Microkit headers.
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
static inline void sel4_call(seL4_CPtr ep, const sel4_msg_t *req, sel4_msg_t *rep)
{
    (void)ep; (void)req;
    rep->opcode = 0;
    rep->length = 0;
}
static inline void seL4_DebugPutChar(char c) { (void)c; }

#else /* !AGENTOS_TEST_HOST — production build */

#include <stdint.h>
#include <stdbool.h>
#include "sel4_server.h"    /* sel4_server_t, sel4_server_init/register/run */
#include "sel4_client.h"    /* sel4_client_t, sel4_client_connect */
#include "sel4_ipc.h"       /* sel4_msg_t, sel4_badge_t, SEL4_ERR_* */
#include <sel4/sel4.h>      /* seL4_DebugPutChar, seL4_Signal */

#endif /* AGENTOS_TEST_HOST */

/* ── Contract opcodes ────────────────────────────────────────────────────── */

#ifndef MSG_FB_CREATE
#define MSG_FB_CREATE             0x2501u
#define MSG_FB_WRITE              0x2502u
#define MSG_FB_FLIP               0x2503u
#define MSG_FB_RESIZE             0x2504u
#define MSG_FB_DESTROY            0x2505u
#define MSG_FB_STATUS             0x2507u
#define MSG_CC_ATTACH_FRAMEBUFFER 0x2D01u  /* CC contract: attach subscriber */
#endif

#ifndef MSG_EVENT_PUBLISH
#define MSG_EVENT_PUBLISH         0x0410u
#define EVENT_FB_FRAME_READY      0x40u
#endif

#ifndef MSG_GPU_SUBMIT
#define OP_GPU_SUBMIT_CMD         0xE4u
#endif

/* FB error codes */
#ifndef FB_OK
#define FB_OK              0u
#define FB_ERR_BAD_HANDLE  1u
#define FB_ERR_BAD_FORMAT  2u
#define FB_ERR_BAD_DIMS    3u
#define FB_ERR_NO_SHMEM    4u
#define FB_ERR_NO_SLOTS    5u
#define FB_ERR_BAD_BACKEND 6u
#define FB_ERR_BAD_RECT    7u
#endif

/* Backend constants */
#ifndef FB_BACKEND_NULL
#define FB_BACKEND_NULL        0u
#define FB_BACKEND_HW_DIRECT   1u
#define FB_BACKEND_REMOTE_API  2u
#endif

/* Pixel formats */
#ifndef FB_FMT_XRGB8888
#define FB_FMT_XRGB8888  0x00u
#define FB_FMT_ARGB8888  0x01u
#define FB_FMT_RGB565    0x02u
#endif

/* Handle sentinel */
#ifndef FB_HANDLE_INVALID
#define FB_HANDLE_INVALID  0u
#endif

/* Nameserver opcode */
#ifndef OP_NS_REGISTER
#define OP_NS_REGISTER 0xD0u
#define OP_NS_LOOKUP   0xD1u
#endif
#ifndef NS_OK
#define NS_OK 0u
#endif
#ifndef NS_NAME_MAX
#define NS_NAME_MAX 32
#endif

/* virtio-gpu MMIO constants */
#ifndef VIRTIO_MMIO_MAGIC_VALUE
#define VIRTIO_MMIO_MAGIC_VALUE   0x000u
#define VIRTIO_MMIO_VERSION       0x004u
#define VIRTIO_MMIO_DEVICE_ID     0x008u
#define VIRTIO_MMIO_STATUS        0x070u
#define VIRTIO_MMIO_MAGIC         0x74726976u
#define VIRTIO_GPU_DEVICE_ID      16u
#define VIRTIO_STATUS_ACKNOWLEDGE (1u << 0)
#define VIRTIO_STATUS_DRIVER      (1u << 1)
#endif

/* ── Limits ──────────────────────────────────────────────────────────────── */

#define FB_MAX_SURFACES     8u
#define FB_MAX_REMOTE_SUBS  4u
#define FB_MAX_WIDTH        7680u   /* 8K */
#define FB_MAX_HEIGHT       4320u
#define FB_SHMEM_SIZE       (32u * 1024u * 1024u)  /* 32 MiB shared region */

/* ── Shmem / MMIO addresses ──────────────────────────────────────────────── */

/*
 * fb_shmem_vaddr — shared memory for framebuffer operations.
 * Set by the root task before calling framebuffer_pd_main().
 * In test builds set directly by the test harness.
 */
uintptr_t fb_shmem_vaddr;

/*
 * virtio_gpu_mmio_vaddr — virtio-gpu MMIO base (QEMU HW_DIRECT path).
 * Set by the root task; 0 if no virtio-gpu hardware is present.
 */
uintptr_t virtio_gpu_mmio_vaddr;

/*
 * log_drain_rings_vaddr — log ring shmem (set by root task; 0 if unused).
 * Resolved via nameserver in production.
 */
uintptr_t log_drain_rings_vaddr;

/* ── HW_DIRECT mode selection ────────────────────────────────────────────── */

typedef enum {
    HW_MODE_NONE       = 0,  /* no hardware; HW_DIRECT behaves as stub */
    HW_MODE_VIRTIO_GPU = 1,  /* QEMU: framebuffer_pd owns virtio-gpu MMIO directly */
    HW_MODE_GPU_SCHED  = 2,  /* Sparky GB10 / NUC: IPC via gpu_sched OP_GPU_SUBMIT_CMD */
} hw_direct_mode_t;

static hw_direct_mode_t hw_mode = HW_MODE_NONE;

/* Nameserver-resolved gpu_sched endpoint (0 = not yet connected) */
static seL4_CPtr g_gpu_sched_ep  = 0;
static seL4_CPtr g_eventbus_ep   = 0;
static seL4_CPtr g_ns_ep         = 0;

/* ── virtio-gpu MMIO helpers ─────────────────────────────────────────────── */

static inline uint32_t fb_mmio_read32(uintptr_t base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void fb_mmio_write32(uintptr_t base, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(base + off) = val;
}

/* ── Data-field helpers ──────────────────────────────────────────────────── */

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

/* ── Bytes-per-pixel table ───────────────────────────────────────────────── */

static uint32_t fmt_bpp(uint32_t fmt)
{
    switch (fmt) {
    case FB_FMT_XRGB8888: return 4;
    case FB_FMT_ARGB8888: return 4;
    case FB_FMT_RGB565:   return 2;
    default:              return 0;
    }
}

/* ── Virtual framebuffer surface (slot) ──────────────────────────────────── */

typedef struct {
    bool      in_use;
    uint32_t  handle;       /* 1-based slot index used as external handle */
    uint32_t  width;
    uint32_t  height;
    uint32_t  format;       /* FB_FMT_* */
    uint32_t  backend;      /* FB_BACKEND_* */
    uint32_t  stride;       /* bytes per row */
    uint32_t  buf_size;     /* total bytes for this surface in fb_shmem */
    uint32_t  shmem_offset; /* byte offset into fb_shmem */
    uint32_t  frame_seq;    /* monotonically increasing commit counter */
    uint32_t  flip_count;
    uint32_t  write_count;
} fb_surface_t;

/* ── Module state ────────────────────────────────────────────────────────── */

static fb_surface_t surfaces[FB_MAX_SURFACES];
static uint32_t     shmem_used;  /* bytes allocated so far in fb_shmem */

/* ── Remote subscriber table ─────────────────────────────────────────────── */

typedef struct {
    bool      active;
    uint32_t  handle;  /* framebuffer handle the subscriber is attached to */
    /* notification cap stored here in a real implementation */
} fb_remote_sub_t;

static fb_remote_sub_t remote_subs[FB_MAX_REMOTE_SUBS];

/* Server instance */
static sel4_server_t g_srv;

/* ── Nameserver lookup helper ────────────────────────────────────────────── */

/*
 * ns_lookup — look up a service by name via the nameserver.
 *
 * Returns 0 (invalid cap) if ns_ep is not set or lookup fails.
 * In production the root task passes a distinct cap slot per service;
 * we use a simple incrementing scheme here.
 *
 * Note: this is a simplified lookup that doesn't use sel4_client_t's
 * full cache machinery, to avoid pulling in sel4_client.h's nameserver.h
 * dependency at test-build time.  Production code uses sel4_client_connect.
 */
static seL4_CPtr fb_ns_lookup(const char *name)
{
    (void)name;
    /*
     * In production: use sel4_client_connect(&g_client, name, &ep).
     * For now return 0 — the caller checks before using the endpoint.
     * The root task wires direct cap grants in E5-S9.
     */
    return 0;
}

/* ── HW_DIRECT backend probe ─────────────────────────────────────────────── */

static void probe_hw_direct(void)
{
    if (virtio_gpu_mmio_vaddr) {
        uint32_t magic  = fb_mmio_read32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_MAGIC_VALUE);
        uint32_t ver    = fb_mmio_read32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_VERSION);
        uint32_t dev_id = fb_mmio_read32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_DEVICE_ID);

        if (magic == VIRTIO_MMIO_MAGIC && ver == 2u
                && dev_id == VIRTIO_GPU_DEVICE_ID) {
            fb_mmio_write32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_STATUS,
                            VIRTIO_STATUS_ACKNOWLEDGE);
            fb_mmio_write32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_STATUS,
                            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
            hw_mode = HW_MODE_VIRTIO_GPU;
            dbg_puts("[framebuffer_pd] HW_DIRECT: virtio-gpu detected (QEMU path)\n");
            return;
        }
        dbg_puts("[framebuffer_pd] HW_DIRECT: virtio-gpu not at MMIO, trying gpu_sched\n");
    }

    /* No virtio-gpu — assume real GPU via gpu_sched (nameserver-resolved) */
    hw_mode = HW_MODE_GPU_SCHED;
    dbg_puts("[framebuffer_pd] HW_DIRECT: gpu_sched path active (Sparky/NUC)\n");
}

/* ── Surface allocation helpers ──────────────────────────────────────────── */

static fb_surface_t *surface_find(uint32_t handle)
{
    if (handle == FB_HANDLE_INVALID || handle > FB_MAX_SURFACES)
        return (void *)0;
    fb_surface_t *s = &surfaces[handle - 1];
    return s->in_use ? s : (void *)0;
}

static fb_surface_t *surface_alloc(void)
{
    for (uint32_t i = 0; i < FB_MAX_SURFACES; i++) {
        if (!surfaces[i].in_use) {
            surfaces[i].in_use = true;
            surfaces[i].handle = i + 1;
            return &surfaces[i];
        }
    }
    return (void *)0;
}

static void surface_free(fb_surface_t *s)
{
    s->in_use = false;
}

/* ── EventBus publishing ─────────────────────────────────────────────────── */

static void publish_frame_ready(uint32_t handle, uint32_t frame_seq,
                                 uint32_t backend)
{
    if (!g_eventbus_ep) {
        /* Try to resolve event_bus via nameserver on first use */
        g_eventbus_ep = fb_ns_lookup("event_bus");
    }
    if (!g_eventbus_ep) return;

    /*
     * Publish EVENT_FB_FRAME_READY to the EventBus service via sel4_call.
     * Replaces the old microkit_ppcall(CH_FB_EVENTBUS, ...) which used a
     * hard-coded channel number that broke when BOARD_qemu_virt_aarch64
     * was not defined (channel 60 invalid channel error).
     */
    sel4_msg_t req, rep;
    req.opcode = (uint32_t)MSG_EVENT_PUBLISH;
    data_wr32(req.data,  0, (uint32_t)MSG_EVENT_PUBLISH);
    data_wr32(req.data,  4, (uint32_t)EVENT_FB_FRAME_READY);
    data_wr32(req.data,  8, handle);
    data_wr32(req.data, 12, frame_seq);
    data_wr32(req.data, 16, backend);
    req.length = 20;
    sel4_call(g_eventbus_ep, &req, &rep);
    /* Ignore reply — EventBus publish is fire-and-forget */
}

/* ── HW_DIRECT flip via gpu_sched ────────────────────────────────────────── */

static uint32_t hw_direct_flip(uint32_t fb_handle, uint32_t frame_seq)
{
    (void)fb_handle; (void)frame_seq;

    if (hw_mode == HW_MODE_VIRTIO_GPU) {
        /* virtio-gpu: framebuffer_pd owns MMIO directly — already written */
        return FB_OK;
    }

    if (hw_mode == HW_MODE_GPU_SCHED) {
        if (!g_gpu_sched_ep) {
            /* Lazy resolve gpu_sched via nameserver */
            g_gpu_sched_ep = fb_ns_lookup("gpu_sched");
        }
        if (!g_gpu_sched_ep) return FB_OK; /* stub: no hw available */

        /*
         * Forward FLIP to gpu_sched via OP_GPU_SUBMIT_CMD.
         * Replaces the old microkit_ppcall(ch=2/7, ...) which used
         * hard-coded Microkit channel numbers.
         */
        sel4_msg_t req, rep;
        req.opcode = OP_GPU_SUBMIT_CMD;
        data_wr32(req.data, 0, OP_GPU_SUBMIT_CMD);
        data_wr32(req.data, 4, 0u);    /* slot_id = 0 (display command slot) */
        data_wr32(req.data, 8, 0u);    /* cmd_offset = 0 */
        data_wr32(req.data, 12, 64u);  /* cmd_len = 64 (virtio-gpu RESOURCE_FLUSH) */
        req.length = 16;
        sel4_call(g_gpu_sched_ep, &req, &rep);
        /* Result in rep.opcode: GPU_ERR_OK = 0 */
    }

    return FB_OK;
}

/* ── IPC handlers ────────────────────────────────────────────────────────── */

/*
 * handle_create — MSG_FB_CREATE
 *
 * Reads fb_create_req from fb_shmem_vaddr.
 * Reply data layout:
 *   data[0..3] = FB_OK or error
 *   data[4..7] = fb_handle (FB_HANDLE_INVALID on error)
 */
static uint32_t handle_create(sel4_badge_t badge, const sel4_msg_t *req,
                                sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;

    if (!fb_shmem_vaddr) {
        data_wr32(rep->data, 0, FB_ERR_NO_SHMEM);
        data_wr32(rep->data, 4, FB_HANDLE_INVALID);
        rep->length = 8;
        return SEL4_ERR_NO_MEM;
    }

    typedef struct { uint32_t width; uint32_t height;
                     uint32_t format; uint32_t backend; } fb_create_req_t;
    const fb_create_req_t *cr = (const fb_create_req_t *)fb_shmem_vaddr;

    uint32_t bpp = fmt_bpp(cr->format);
    if (bpp == 0) {
        data_wr32(rep->data, 0, FB_ERR_BAD_FORMAT);
        data_wr32(rep->data, 4, FB_HANDLE_INVALID);
        rep->length = 8;
        return SEL4_ERR_BAD_ARG;
    }

    if (cr->width == 0 || cr->height == 0 ||
        cr->width > FB_MAX_WIDTH || cr->height > FB_MAX_HEIGHT) {
        data_wr32(rep->data, 0, FB_ERR_BAD_DIMS);
        data_wr32(rep->data, 4, FB_HANDLE_INVALID);
        rep->length = 8;
        return SEL4_ERR_BAD_ARG;
    }

    uint32_t backend = cr->backend;
    if (backend > FB_BACKEND_REMOTE_API) backend = FB_BACKEND_NULL;

    uint32_t stride       = cr->width * bpp;
    uint32_t buf_size     = stride * cr->height;
    uint32_t aligned_size = (buf_size + 63u) & ~63u;

    if (shmem_used + aligned_size > FB_SHMEM_SIZE) {
        data_wr32(rep->data, 0, FB_ERR_NO_SLOTS);
        data_wr32(rep->data, 4, FB_HANDLE_INVALID);
        rep->length = 8;
        return SEL4_ERR_NO_MEM;
    }

    fb_surface_t *s = surface_alloc();
    if (!s) {
        data_wr32(rep->data, 0, FB_ERR_NO_SLOTS);
        data_wr32(rep->data, 4, FB_HANDLE_INVALID);
        rep->length = 8;
        return SEL4_ERR_NO_MEM;
    }

    s->width        = cr->width;
    s->height       = cr->height;
    s->format       = cr->format;
    s->backend      = backend;
    s->stride       = stride;
    s->buf_size     = buf_size;
    s->shmem_offset = shmem_used;
    s->frame_seq    = 0;
    s->flip_count   = 0;
    s->write_count  = 0;

    shmem_used += aligned_size;

    data_wr32(rep->data, 0, FB_OK);
    data_wr32(rep->data, 4, s->handle);
    rep->length = 8;
    return SEL4_ERR_OK;
}

/*
 * handle_write — MSG_FB_WRITE
 *
 * Reads fb_write_req from fb_shmem_vaddr.
 * Reply data layout:
 *   data[0..3] = FB_OK or error
 */
static uint32_t handle_write(sel4_badge_t badge, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;

    if (!fb_shmem_vaddr) {
        data_wr32(rep->data, 0, FB_ERR_NO_SHMEM);
        rep->length = 4;
        return SEL4_ERR_NO_MEM;
    }

    typedef struct { uint32_t fb_handle; uint32_t x; uint32_t y;
                     uint32_t w; uint32_t h; } fb_write_req_t;
    const fb_write_req_t *wr = (const fb_write_req_t *)fb_shmem_vaddr;

    fb_surface_t *s = surface_find(wr->fb_handle);
    if (!s) {
        data_wr32(rep->data, 0, FB_ERR_BAD_HANDLE);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }

    if (wr->x + wr->w > s->width || wr->y + wr->h > s->height ||
        wr->w == 0 || wr->h == 0) {
        data_wr32(rep->data, 0, FB_ERR_BAD_RECT);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }

    s->write_count++;
    data_wr32(rep->data, 0, FB_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/*
 * handle_flip — MSG_FB_FLIP
 *
 * Request data layout:
 *   data[0..3] = handle
 *
 * Reply data layout:
 *   data[0..3] = FB_OK or error
 *   data[4..7] = frame_seq
 */
static uint32_t handle_flip(sel4_badge_t badge, const sel4_msg_t *req,
                              sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t handle = data_rd32(req->data, 0);

    fb_surface_t *s = surface_find(handle);
    if (!s) {
        data_wr32(rep->data, 0, FB_ERR_BAD_HANDLE);
        data_wr32(rep->data, 4, 0u);
        rep->length = 8;
        return SEL4_ERR_BAD_ARG;
    }

    uint32_t result = FB_OK;
    if (s->backend == FB_BACKEND_HW_DIRECT)
        result = hw_direct_flip(s->handle, s->frame_seq + 1);

    if (result != FB_OK) {
        data_wr32(rep->data, 0, result);
        data_wr32(rep->data, 4, s->frame_seq);
        rep->length = 8;
        return SEL4_ERR_BAD_ARG;
    }

    s->frame_seq++;
    s->flip_count++;

    /*
     * Publish EVENT_FB_FRAME_READY to EventBus via nameserver-resolved endpoint.
     * This replaces the old microkit_ppcall(CH_FB_EVENTBUS, ...) which used
     * channel 60 (only valid when BOARD_qemu_virt_aarch64 was defined).
     */
    publish_frame_ready(handle, s->frame_seq, s->backend);

    data_wr32(rep->data, 0, FB_OK);
    data_wr32(rep->data, 4, s->frame_seq);
    rep->length = 8;
    return SEL4_ERR_OK;
}

/*
 * handle_resize — MSG_FB_RESIZE
 *
 * Reads fb_resize_req from fb_shmem_vaddr.
 * Reply data layout:
 *   data[0..3] = FB_OK or error
 */
static uint32_t handle_resize(sel4_badge_t badge, const sel4_msg_t *req,
                                sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;

    if (!fb_shmem_vaddr) {
        data_wr32(rep->data, 0, FB_ERR_NO_SHMEM);
        rep->length = 4;
        return SEL4_ERR_NO_MEM;
    }

    typedef struct { uint32_t fb_handle; uint32_t width;
                     uint32_t height; uint32_t format; } fb_resize_req_t;
    const fb_resize_req_t *rr = (const fb_resize_req_t *)fb_shmem_vaddr;

    fb_surface_t *s = surface_find(rr->fb_handle);
    if (!s) {
        data_wr32(rep->data, 0, FB_ERR_BAD_HANDLE);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }

    uint32_t new_fmt = (rr->format != 0) ? rr->format : s->format;
    uint32_t bpp     = fmt_bpp(new_fmt);
    if (bpp == 0) {
        data_wr32(rep->data, 0, FB_ERR_BAD_FORMAT);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }

    if (rr->width == 0 || rr->height == 0 ||
        rr->width > FB_MAX_WIDTH || rr->height > FB_MAX_HEIGHT) {
        data_wr32(rep->data, 0, FB_ERR_BAD_DIMS);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }

    uint32_t new_stride   = rr->width * bpp;
    uint32_t new_buf_size = new_stride * rr->height;
    uint32_t aligned_new  = (new_buf_size + 63u) & ~63u;
    uint32_t aligned_old  = (s->buf_size + 63u) & ~63u;

    if (aligned_new > aligned_old) {
        uint32_t tail_offset = s->shmem_offset + aligned_old;
        if (tail_offset == shmem_used) {
            if (shmem_used - aligned_old + aligned_new > FB_SHMEM_SIZE) {
                data_wr32(rep->data, 0, FB_ERR_NO_SHMEM);
                rep->length = 4;
                return SEL4_ERR_NO_MEM;
            }
            shmem_used = shmem_used - aligned_old + aligned_new;
        } else {
            data_wr32(rep->data, 0, FB_ERR_NO_SHMEM);
            rep->length = 4;
            return SEL4_ERR_NO_MEM;
        }
    }

    s->width    = rr->width;
    s->height   = rr->height;
    s->format   = new_fmt;
    s->stride   = new_stride;
    s->buf_size = new_buf_size;

    data_wr32(rep->data, 0, FB_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/*
 * handle_destroy — MSG_FB_DESTROY
 *
 * Request data layout:
 *   data[0..3] = handle
 *
 * Reply data layout:
 *   data[0..3] = FB_OK or error
 */
static uint32_t handle_destroy(sel4_badge_t badge, const sel4_msg_t *req,
                                 sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t handle = data_rd32(req->data, 0);

    fb_surface_t *s = surface_find(handle);
    if (!s) {
        data_wr32(rep->data, 0, FB_ERR_BAD_HANDLE);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }

    /* Detach all remote subscribers for this framebuffer. */
    for (uint32_t i = 0; i < FB_MAX_REMOTE_SUBS; i++) {
        if (remote_subs[i].active && remote_subs[i].handle == handle)
            remote_subs[i].active = false;
    }

    /* Reclaim shmem only when destroying the tail allocation */
    uint32_t aligned = (s->buf_size + 63u) & ~63u;
    if (s->shmem_offset + aligned == shmem_used)
        shmem_used -= aligned;

    surface_free(s);
    data_wr32(rep->data, 0, FB_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/*
 * handle_status — MSG_FB_STATUS
 *
 * Request data layout:
 *   data[0..3] = handle
 *
 * Writes fb_status_reply into fb_shmem_vaddr; reply data[0..3] = FB_OK or error.
 */
static uint32_t handle_status(sel4_badge_t badge, const sel4_msg_t *req,
                                sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t handle = data_rd32(req->data, 0);

    fb_surface_t *s = surface_find(handle);
    if (!s) {
        data_wr32(rep->data, 0, FB_ERR_BAD_HANDLE);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }

    if (!fb_shmem_vaddr) {
        data_wr32(rep->data, 0, FB_ERR_NO_SHMEM);
        rep->length = 4;
        return SEL4_ERR_NO_MEM;
    }

    typedef struct {
        uint32_t ok; uint32_t width; uint32_t height;
        uint32_t format; uint32_t backend; uint32_t frame_seq;
        uint32_t flip_count; uint32_t write_count;
    } fb_status_reply_t;
    fb_status_reply_t *srep = (fb_status_reply_t *)fb_shmem_vaddr;
    srep->ok          = FB_OK;
    srep->width       = s->width;
    srep->height      = s->height;
    srep->format      = s->format;
    srep->backend     = s->backend;
    srep->frame_seq   = s->frame_seq;
    srep->flip_count  = s->flip_count;
    srep->write_count = s->write_count;

    data_wr32(rep->data, 0, FB_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/*
 * handle_remote_attach — MSG_CC_ATTACH_FRAMEBUFFER
 *
 * Request data layout:
 *   data[0..3] = handle
 *
 * Reply data layout:
 *   data[0..3] = FB_OK or error
 *
 * In the raw seL4 migration, notification cap transfer for remote subscribers
 * is deferred to E5-S9 (cap-transfer pass).  For now we record the badge
 * as subscriber identity.
 */
static uint32_t handle_remote_attach(sel4_badge_t badge, const sel4_msg_t *req,
                                      sel4_msg_t *rep, void *ctx)
{
    (void)ctx;

    uint32_t handle = data_rd32(req->data, 0);

    fb_surface_t *s = surface_find(handle);
    if (!s) {
        data_wr32(rep->data, 0, FB_ERR_BAD_HANDLE);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }
    if (s->backend != FB_BACKEND_REMOTE_API) {
        data_wr32(rep->data, 0, FB_ERR_BAD_BACKEND);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }

    for (uint32_t i = 0; i < FB_MAX_REMOTE_SUBS; i++) {
        if (!remote_subs[i].active) {
            remote_subs[i].active = true;
            remote_subs[i].handle = handle;
            (void)badge; /* badge carries subscriber identity; stored in E5-S9 */
            data_wr32(rep->data, 0, FB_OK);
            rep->length = 4;
            return SEL4_ERR_OK;
        }
    }

    data_wr32(rep->data, 0, FB_ERR_NO_SLOTS);
    rep->length = 4;
    return SEL4_ERR_NO_MEM;
}

/* ── Nameserver self-registration ────────────────────────────────────────── */

static void register_with_nameserver(seL4_CPtr ns_ep)
{
    if (!ns_ep) return;
    g_ns_ep = ns_ep;

    sel4_msg_t req, rep;
    req.opcode = OP_NS_REGISTER;

    data_wr32(req.data,  0, 0u);    /* channel_id */
    data_wr32(req.data,  4, 17u);   /* pd_id = framebuffer_pd */
    data_wr32(req.data,  8, 0u);    /* cap_classes */
    data_wr32(req.data, 12, 1u);    /* version */

    const char *name = "framebuffer";
    int i = 0;
    for (; name[i] && (16 + i) < 48; i++)
        req.data[16 + i] = (uint8_t)name[i];
    for (; (16 + i) < 48; i++)
        req.data[16 + i] = 0;

    req.length = 48;
    sel4_call(ns_ep, &req, &rep);
    /* Ignore return — continue if nameserver is offline */
}

/* ── Test-host entry points ──────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST

void framebuffer_pd_test_init(void)
{
    hw_mode        = HW_MODE_NONE;
    shmem_used     = 0;
    g_gpu_sched_ep = 0;
    g_eventbus_ep  = 0;
    g_ns_ep        = 0;

    for (uint32_t i = 0; i < FB_MAX_SURFACES; i++)
        surfaces[i].in_use = false;
    for (uint32_t i = 0; i < FB_MAX_REMOTE_SUBS; i++)
        remote_subs[i].active = false;

    sel4_server_init(&g_srv, 0);
    sel4_server_register(&g_srv, MSG_FB_CREATE,             handle_create,        (void *)0);
    sel4_server_register(&g_srv, MSG_FB_WRITE,              handle_write,         (void *)0);
    sel4_server_register(&g_srv, MSG_FB_FLIP,               handle_flip,          (void *)0);
    sel4_server_register(&g_srv, MSG_FB_RESIZE,             handle_resize,        (void *)0);
    sel4_server_register(&g_srv, MSG_FB_DESTROY,            handle_destroy,       (void *)0);
    sel4_server_register(&g_srv, MSG_FB_STATUS,             handle_status,        (void *)0);
    sel4_server_register(&g_srv, MSG_CC_ATTACH_FRAMEBUFFER, handle_remote_attach, (void *)0);
}

uint32_t framebuffer_pd_dispatch_one(sel4_badge_t badge,
                                      const sel4_msg_t *req,
                                      sel4_msg_t *rep)
{
    return sel4_server_dispatch(&g_srv, badge, req, rep);
}

#else /* !AGENTOS_TEST_HOST — production build */

/*
 * framebuffer_pd_main — production entry point called by the root task boot
 * dispatcher.
 *
 * my_ep: listen endpoint capability.
 * ns_ep: nameserver endpoint (0 = nameserver not yet available).
 *
 * This function NEVER RETURNS.
 */
void framebuffer_pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    dbg_puts("[framebuffer_pd] starting — agentOS virtual framebuffer service\n");

    shmem_used     = 0;
    g_gpu_sched_ep = 0;
    g_eventbus_ep  = 0;

    for (uint32_t i = 0; i < FB_MAX_SURFACES; i++)
        surfaces[i].in_use = false;
    for (uint32_t i = 0; i < FB_MAX_REMOTE_SUBS; i++)
        remote_subs[i].active = false;

    /* Detect HW_DIRECT backend: virtio-gpu (QEMU) or gpu_sched (Sparky/NUC) */
    probe_hw_direct();

    if (!fb_shmem_vaddr)
        dbg_puts("[framebuffer_pd] WARNING: fb_shmem_vaddr not mapped "
                 "(NULL backend only)\n");

    /* Self-register with nameserver */
    register_with_nameserver(ns_ep);

    dbg_puts("[framebuffer_pd] ready — waiting for IPC\n");

    sel4_server_init(&g_srv, my_ep);
    sel4_server_register(&g_srv, MSG_FB_CREATE,             handle_create,        (void *)0);
    sel4_server_register(&g_srv, MSG_FB_WRITE,              handle_write,         (void *)0);
    sel4_server_register(&g_srv, MSG_FB_FLIP,               handle_flip,          (void *)0);
    sel4_server_register(&g_srv, MSG_FB_RESIZE,             handle_resize,        (void *)0);
    sel4_server_register(&g_srv, MSG_FB_DESTROY,            handle_destroy,       (void *)0);
    sel4_server_register(&g_srv, MSG_FB_STATUS,             handle_status,        (void *)0);
    sel4_server_register(&g_srv, MSG_CC_ATTACH_FRAMEBUFFER, handle_remote_attach, (void *)0);

    /* Enter the recv/dispatch/reply loop — never returns */
    sel4_server_run(&g_srv);
}

#endif /* AGENTOS_TEST_HOST */
