/*
 * framebuffer_pd.c — agentOS Framebuffer Protection Domain (Phase 4a core)
 *
 * Manages virtual framebuffers for guest OSes and VibeOS.  Owns the
 * fb_shmem shared memory region; guests blit pixels there then call
 * MSG_FB_FLIP to commit a frame.
 *
 * IPC protocol — opcode in MR0:
 *   MSG_FB_CREATE    fb_create_req in shmem → MR0=ok MR1=fb_handle
 *   MSG_FB_WRITE     fb_write_req in shmem; pixels in fb_shmem → MR0=ok
 *   MSG_FB_FLIP      MR1=fb_handle → MR0=ok MR1=frame_seq
 *   MSG_FB_RESIZE    fb_resize_req in shmem → MR0=ok; fb_resize_reply in shmem
 *   MSG_FB_DESTROY   MR1=fb_handle → MR0=ok
 *   MSG_FB_STATUS    MR1=fb_handle → MR0=ok; fb_status_reply in shmem
 *
 * Backends:
 *   FB_BACKEND_NULL       — frames discarded after FLIP (this file)
 *   FB_BACKEND_HW_DIRECT  — virtio-gpu / display controller (ag-1es, not here)
 *   FB_BACKEND_REMOTE_API — host window-system relay (ag-tz2, not here)
 *
 * Graphics is opt-in.  Guests that never call MSG_FB_CREATE receive no
 * framebuffer capability; no resources are allocated on their behalf.
 *
 * Priority: 160  (between serial_pd and eventbus; real-time display path)
 * Mode: passive  (woken by PPC from callers)
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/framebuffer_contract.h"

/* ─── Limits ─────────────────────────────────────────────────────────────── */

#define FB_MAX_SURFACES   8u
#define FB_MAX_WIDTH      7680u   /* 8K */
#define FB_MAX_HEIGHT     4320u
#define FB_SHMEM_SIZE     (32u * 1024u * 1024u)  /* 32 MiB shared region */

/* ─── Bytes-per-pixel table ─────────────────────────────────────────────── */

static uint32_t fmt_bpp(uint32_t fmt)
{
    switch (fmt) {
    case FB_FMT_XRGB8888: return 4;
    case FB_FMT_ARGB8888: return 4;
    case FB_FMT_RGB565:   return 2;
    default:              return 0;
    }
}

/* ─── Backend dispatch table ─────────────────────────────────────────────── */

typedef struct {
    /*
     * Called on MSG_FB_FLIP.  Returns FB_OK or a FB_ERR_* code.
     * The NULL backend always returns FB_OK.
     */
    uint32_t (*flip)(uint32_t fb_handle, uint32_t frame_seq);
} fb_backend_ops_t;

/* NULL backend — frames are silently discarded */
static uint32_t null_flip(uint32_t fb_handle, uint32_t frame_seq)
{
    (void)fb_handle;
    (void)frame_seq;
    return FB_OK;
}

static const fb_backend_ops_t backend_ops[3] = {
    [FB_BACKEND_NULL]       = { .flip = null_flip },
    [FB_BACKEND_HW_DIRECT]  = { .flip = null_flip },  /* stub — ag-1es fills this */
    [FB_BACKEND_REMOTE_API] = { .flip = null_flip },  /* stub — ag-tz2 fills this */
};

/* ─── Surface table ──────────────────────────────────────────────────────── */

typedef struct {
    bool      in_use;
    uint32_t  handle;       /* 1-based slot index used as external handle */
    uint32_t  width;
    uint32_t  height;
    uint32_t  format;       /* FB_FMT_* */
    uint32_t  backend;      /* fb_backend_t */
    uint32_t  stride;       /* bytes per row */
    uint32_t  buf_size;     /* total bytes for this surface in fb_shmem */
    uint32_t  shmem_offset; /* byte offset into fb_shmem */
    uint32_t  frame_seq;    /* monotonically increasing commit counter */
    uint32_t  flip_count;
    uint32_t  write_count;
} fb_surface_t;

/* ─── Module state ───────────────────────────────────────────────────────── */

uintptr_t fb_shmem_vaddr;   /* set by microkit from system description */

static fb_surface_t surfaces[FB_MAX_SURFACES];
static uint32_t     shmem_used;  /* bytes allocated so far in fb_shmem */

/* ─── Surface allocation helpers ────────────────────────────────────────── */

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

/* ─── IPC handlers ───────────────────────────────────────────────────────── */

static void handle_create(void)
{
    if (!fb_shmem_vaddr) {
        microkit_mr_set(0, FB_ERR_NO_SHMEM);
        microkit_mr_set(1, FB_HANDLE_INVALID);
        return;
    }

    const struct fb_create_req *req = (const struct fb_create_req *)fb_shmem_vaddr;

    uint32_t bpp = fmt_bpp(req->format);
    if (bpp == 0) {
        microkit_mr_set(0, FB_ERR_BAD_FORMAT);
        microkit_mr_set(1, FB_HANDLE_INVALID);
        return;
    }

    if (req->width == 0 || req->height == 0 ||
        req->width > FB_MAX_WIDTH || req->height > FB_MAX_HEIGHT) {
        microkit_mr_set(0, FB_ERR_BAD_DIMS);
        microkit_mr_set(1, FB_HANDLE_INVALID);
        return;
    }

    uint32_t backend = req->backend;
    if (backend > FB_BACKEND_REMOTE_API)
        backend = FB_BACKEND_NULL;

    uint32_t stride   = req->width * bpp;
    uint32_t buf_size = stride * req->height;

    /* Align allocation to 64-byte cache line */
    uint32_t aligned_size = (buf_size + 63u) & ~63u;

    if (shmem_used + aligned_size > FB_SHMEM_SIZE) {
        microkit_mr_set(0, FB_ERR_NO_SLOTS);
        microkit_mr_set(1, FB_HANDLE_INVALID);
        return;
    }

    fb_surface_t *s = surface_alloc();
    if (!s) {
        microkit_mr_set(0, FB_ERR_NO_SLOTS);
        microkit_mr_set(1, FB_HANDLE_INVALID);
        return;
    }

    s->width        = req->width;
    s->height       = req->height;
    s->format       = req->format;
    s->backend      = backend;
    s->stride       = stride;
    s->buf_size     = buf_size;
    s->shmem_offset = shmem_used;
    s->frame_seq    = 0;
    s->flip_count   = 0;
    s->write_count  = 0;

    shmem_used += aligned_size;

    /* Write reply struct into shmem after the request */
    struct fb_create_reply *rep =
        (struct fb_create_reply *)(fb_shmem_vaddr + sizeof(struct fb_create_req));
    rep->ok             = FB_OK;
    rep->fb_handle      = s->handle;
    rep->actual_backend = s->backend;
    rep->stride         = s->stride;
    rep->buf_size       = s->buf_size;
    rep->shmem_offset   = s->shmem_offset;

    microkit_mr_set(0, FB_OK);
    microkit_mr_set(1, s->handle);
}

static void handle_write(void)
{
    if (!fb_shmem_vaddr) {
        microkit_mr_set(0, FB_ERR_NO_SHMEM);
        return;
    }

    const struct fb_write_req *req = (const struct fb_write_req *)fb_shmem_vaddr;

    fb_surface_t *s = surface_find(req->fb_handle);
    if (!s) {
        microkit_mr_set(0, FB_ERR_BAD_HANDLE);
        return;
    }

    /* Validate destination rectangle */
    if (req->x + req->w > s->width || req->y + req->h > s->height ||
        req->w == 0 || req->h == 0) {
        microkit_mr_set(0, FB_ERR_BAD_RECT);
        return;
    }

    /*
     * Pixel data is already at fb_shmem_vaddr + s->shmem_offset (placed there
     * by the caller before sending MSG_FB_WRITE).  No copy is needed: both
     * sides share the same physical memory region via seL4 shared MR.
     * We simply validate bounds above and count the write.
     */
    s->write_count++;
    microkit_mr_set(0, FB_OK);
}

static void handle_flip(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);

    fb_surface_t *s = surface_find(handle);
    if (!s) {
        microkit_mr_set(0, FB_ERR_BAD_HANDLE);
        microkit_mr_set(1, 0);
        return;
    }

    uint32_t result = backend_ops[s->backend].flip(s->handle, s->frame_seq + 1);
    if (result != FB_OK) {
        microkit_mr_set(0, result);
        microkit_mr_set(1, s->frame_seq);
        return;
    }

    s->frame_seq++;
    s->flip_count++;

    microkit_mr_set(0, FB_OK);
    microkit_mr_set(1, s->frame_seq);

    /*
     * Publish MSG_FB_FRAME_READY to EventBus so subscribers (e.g. cc_pd)
     * learn a new frame is available without polling.
     *
     * We use a fire-and-forget notify rather than a PPC to avoid blocking
     * the framebuffer_pd in the display hot path.
     */
    microkit_mr_set(0, EVT_FB_FRAME_READY);
    microkit_mr_set(1, s->handle);
    microkit_mr_set(2, s->frame_seq);
    microkit_mr_set(3, s->width);
    microkit_mr_set(4, s->height);
    microkit_mr_set(5, s->format);
    microkit_notify(EVENTBUS_CH_MONITOR);  /* EventBus is on channel 1 from our view */

    /* Restore reply registers after notify clobbered MR0 */
    microkit_mr_set(0, FB_OK);
    microkit_mr_set(1, s->frame_seq);
}

static void handle_resize(void)
{
    if (!fb_shmem_vaddr) {
        microkit_mr_set(0, FB_ERR_NO_SHMEM);
        return;
    }

    const struct fb_resize_req *req = (const struct fb_resize_req *)fb_shmem_vaddr;

    fb_surface_t *s = surface_find(req->fb_handle);
    if (!s) {
        microkit_mr_set(0, FB_ERR_BAD_HANDLE);
        return;
    }

    uint32_t new_fmt = (req->format != 0) ? req->format : s->format;
    uint32_t bpp     = fmt_bpp(new_fmt);
    if (bpp == 0) {
        microkit_mr_set(0, FB_ERR_BAD_FORMAT);
        return;
    }

    if (req->width == 0 || req->height == 0 ||
        req->width > FB_MAX_WIDTH || req->height > FB_MAX_HEIGHT) {
        microkit_mr_set(0, FB_ERR_BAD_DIMS);
        return;
    }

    uint32_t new_stride   = req->width * bpp;
    uint32_t new_buf_size = new_stride * req->height;
    uint32_t aligned_new  = (new_buf_size + 63u) & ~63u;
    uint32_t aligned_old  = (s->buf_size + 63u) & ~63u;

    if (aligned_new > aligned_old) {
        /* Need more space than currently allocated: check tail headroom */
        uint32_t tail_offset = s->shmem_offset + aligned_old;
        if (tail_offset == shmem_used) {
            /* Surface is at the tail — can expand in place */
            if (shmem_used - aligned_old + aligned_new > FB_SHMEM_SIZE) {
                microkit_mr_set(0, FB_ERR_NO_SHMEM);
                return;
            }
            shmem_used = shmem_used - aligned_old + aligned_new;
        } else {
            /* Mid-table surface — cannot resize in place without compaction */
            microkit_mr_set(0, FB_ERR_NO_SHMEM);
            return;
        }
    }

    s->width    = req->width;
    s->height   = req->height;
    s->format   = new_fmt;
    s->stride   = new_stride;
    s->buf_size = new_buf_size;

    struct fb_resize_reply *rep =
        (struct fb_resize_reply *)(fb_shmem_vaddr + sizeof(struct fb_resize_req));
    rep->ok           = FB_OK;
    rep->actual_width = s->width;
    rep->actual_height= s->height;
    rep->stride       = s->stride;
    rep->buf_size     = s->buf_size;
    rep->shmem_offset = s->shmem_offset;

    microkit_mr_set(0, FB_OK);
}

static void handle_destroy(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);

    fb_surface_t *s = surface_find(handle);
    if (!s) {
        microkit_mr_set(0, FB_ERR_BAD_HANDLE);
        return;
    }

    /* Reclaim shmem only when destroying the tail allocation */
    uint32_t aligned = (s->buf_size + 63u) & ~63u;
    if (s->shmem_offset + aligned == shmem_used)
        shmem_used -= aligned;

    surface_free(s);
    microkit_mr_set(0, FB_OK);
}

static void handle_status(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);

    fb_surface_t *s = surface_find(handle);
    if (!s) {
        microkit_mr_set(0, FB_ERR_BAD_HANDLE);
        return;
    }

    if (!fb_shmem_vaddr) {
        microkit_mr_set(0, FB_ERR_NO_SHMEM);
        return;
    }

    struct fb_status_reply *rep = (struct fb_status_reply *)fb_shmem_vaddr;
    rep->ok          = FB_OK;
    rep->width       = s->width;
    rep->height      = s->height;
    rep->format      = s->format;
    rep->backend     = s->backend;
    rep->frame_seq   = s->frame_seq;
    rep->flip_count  = s->flip_count;
    rep->write_count = s->write_count;

    microkit_mr_set(0, FB_OK);
}

/* ─── Microkit entry points ──────────────────────────────────────────────── */

void init(void)
{
    microkit_dbg_puts("[framebuffer_pd] starting — agentOS framebuffer service\n");

    for (uint32_t i = 0; i < FB_MAX_SURFACES; i++)
        surfaces[i].in_use = false;
    shmem_used = 0;

    if (!fb_shmem_vaddr) {
        microkit_dbg_puts("[framebuffer_pd] WARNING: fb_shmem_vaddr not mapped "
                          "(NULL backend only)\n");
    }

    microkit_dbg_puts("[framebuffer_pd] ready — NULL backend active\n");
}

void notified(microkit_channel ch)
{
    (void)ch;
    /* No async notifications expected in Phase 4a core */
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch;
    (void)msginfo;

    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {
    case MSG_FB_CREATE:  handle_create();  break;
    case MSG_FB_WRITE:   handle_write();   break;
    case MSG_FB_FLIP:    handle_flip();    break;
    case MSG_FB_RESIZE:  handle_resize();  break;
    case MSG_FB_DESTROY: handle_destroy(); break;
    case MSG_FB_STATUS:  handle_status();  break;
    default:
        microkit_dbg_puts("[framebuffer_pd] unknown opcode\n");
        microkit_mr_set(0, FB_ERR_BAD_HANDLE);
        break;
    }

    return microkit_msginfo_new(0, 6);
}
