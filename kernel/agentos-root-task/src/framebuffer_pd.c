/*
 * framebuffer_pd.c — agentOS Virtual Framebuffer Protection Domain
 *
 * Manages virtual framebuffers for guest OSes via a backend dispatch model.
 * Each guest selects a backend at MSG_FB_CREATE time:
 *
 *   FB_BACKEND_NULL       — frames discarded; for headless guests
 *   FB_BACKEND_HW_DIRECT  — virtio-gpu / gpu_sched (Phase 4a-hw, ag-1es)
 *   FB_BACKEND_REMOTE_API — display server relay (Phase 4a-remote, ag-tz2)
 *
 * IPC protocol (opcode in MR0):
 *   MSG_FB_CREATE              fb_create_req in shmem → MR0=ok MR1=handle
 *   MSG_FB_WRITE               MR1=handle; fb_write_req in shmem → MR0=ok
 *   MSG_FB_FLIP                MR1=handle → MR0=ok MR1=frame_seq
 *   MSG_FB_RESIZE              MR1=handle; fb_resize_req in shmem → MR0=ok
 *   MSG_FB_DESTROY             MR1=handle → MR0=ok
 *   MSG_FB_STATUS              MR1=handle → MR0=ok; fb_status_reply in shmem
 *   MSG_CC_ATTACH_FRAMEBUFFER  MR1=handle → MR0=ok; register caller as subscriber
 *
 * REMOTE_API backend (Phase 4a-remote):
 *   On MSG_FB_FLIP the REMOTE_API backend forwards the committed frame to
 *   external subscribers via microkit_notify().  Subscribers registered via
 *   MSG_CC_ATTACH_FRAMEBUFFER have their notified() called; they then read
 *   pixel data directly from the shared fb_shmem region.  Up to
 *   FB_MAX_REMOTE_SUBS concurrent subscribers per system are supported.
 *
 * EventBus integration:
 *   Every successful FLIP (any backend) publishes EVENT_FB_FRAME_READY to
 *   EventBus on CH_FB_EVENTBUS, provided the channel is wired in the
 *   system manifest (fb_eventbus_wired_flag != 0).
 *
 * Guest binding:
 *   MSG_GUEST_BIND_DEVICE(GUEST_DEV_FB) carries the handle from MSG_FB_CREATE
 *   as dev_handle.  The fb_token in guest_capabilities_t is set by the
 *   root-task after validating the handle (see guest_contract.h).
 *
 * Shared memory (fb_shmem_vaddr):
 *   Callers write request structs and pixel data here before calling MSG_FB_*.
 *   The region is large enough for one full-HD frame (≥ 1920×1080×4 bytes).
 *
 * Channel: CH_FB_PD (see agentos.h) — inbound PPC from callers (local id=0)
 * Channel 1: CH_FB_EVENTBUS — outbound to EventBus for FRAME_READY (optional)
 * Channels 2..5: remote subscriber notification channels (optional)
 * Priority: 175  (below serial; above idle workers)
 * Mode: passive  (woken by PPC from callers)
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "contracts/framebuffer_contract.h"
#include "contracts/cc_contract.h"
#include "gpu_sched.h"

/* ─── Channel assignments ────────────────────────────────────────────────── */

#define CH_FB_EVENTBUS      1u   /* optional: framebuffer_pd → EventBus (PPC) */
#define CH_FB_GPU_SCHED     2u   /* framebuffer_pd → gpu_sched (Sparky real-GPU path) */
#define CH_FB_REMOTE_BASE   2u   /* remote subscriber channels: 2..5 */

/* ─── Limits ─────────────────────────────────────────────────────────────── */

#define FB_MAX_SURFACES     8u
#define FB_MAX_CLIENTS      8u
#define FB_MAX_REMOTE_SUBS  4u
#define FB_MAX_WIDTH        7680u   /* 8K */
#define FB_MAX_HEIGHT       4320u
#define FB_SHMEM_SIZE       (32u * 1024u * 1024u)  /* 32 MiB shared region */

/* ─── Shmem / setvar (Microkit setvar_vaddr) ─────────────────────────────── */

uintptr_t fb_shmem_vaddr;

/*
 * Set to non-zero by Microkit setvar_vaddr="fb_eventbus_wired_flag" in system
 * manifests that wire the EventBus channel (CH_FB_EVENTBUS).  Defaults to 0
 * (BSS) in manifests that do not include EventBus (e.g. agentos-freebsd.system).
 */
uintptr_t fb_eventbus_wired_flag;

/* virtio-gpu MMIO base: set by Microkit setvar_vaddr (QEMU HW_DIRECT path) */
uintptr_t virtio_gpu_mmio_vaddr;

/* Declared for log_drain_write(); falls back to microkit_dbg_puts if unmapped */
uintptr_t log_drain_rings_vaddr;

/* ─── HW_DIRECT mode selection ───────────────────────────────────────────── */

typedef enum {
    HW_MODE_NONE       = 0,  /* no hardware; HW_DIRECT behaves as stub */
    HW_MODE_VIRTIO_GPU = 1,  /* QEMU: framebuffer_pd owns virtio-gpu MMIO directly */
    HW_MODE_GPU_SCHED  = 2,  /* Sparky GB10 / NUC: IPC via gpu_sched OP_GPU_SUBMIT_CMD */
} hw_direct_mode_t;

static hw_direct_mode_t hw_mode = HW_MODE_NONE;

/* ─── virtio-gpu MMIO helpers ────────────────────────────────────────────── */

static inline uint32_t fb_mmio_read32(uintptr_t base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void fb_mmio_write32(uintptr_t base, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(base + off) = val;
}

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

/* ─── Virtual framebuffer surface (slot) ─────────────────────────────────── */

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

static fb_surface_t surfaces[FB_MAX_SURFACES];
static uint32_t     shmem_used;  /* bytes allocated so far in fb_shmem */
static bool         eventbus_connected = false;

/* ─── Remote subscriber table ────────────────────────────────────────────── */

typedef struct {
    bool             active;
    uint32_t ch;      /* calling channel that registered this subscriber */
    uint32_t         handle;  /* framebuffer handle the subscriber is attached to */
} fb_remote_sub_t;

static fb_remote_sub_t remote_subs[FB_MAX_REMOTE_SUBS];

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

/* ─── HW_DIRECT backend (Phase 4a-hw, ag-1es) ───────────────────────────── */

/*
 * probe_hw_direct — detect available display hardware at init time.
 *
 * Checks virtio-gpu MMIO (QEMU path) first.  If the device is not present,
 * falls back to the Sparky GB10 / NUC path via gpu_sched IPC.
 * Sets hw_mode accordingly; HW_DIRECT operations dispatch on hw_mode.
 */
static void probe_hw_direct(void)
{
    if (virtio_gpu_mmio_vaddr) {
        uint32_t magic  = fb_mmio_read32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_MAGIC_VALUE);
        uint32_t ver    = fb_mmio_read32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_VERSION);
        uint32_t dev_id = fb_mmio_read32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_DEVICE_ID);

        if (magic == VIRTIO_MMIO_MAGIC && ver == 2u
                && dev_id == VIRTIO_GPU_DEVICE_ID) {
            /* Minimal virtio initialisation: ACKNOWLEDGE → DRIVER */
            fb_mmio_write32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_STATUS,
                            VIRTIO_STATUS_ACKNOWLEDGE);
            fb_mmio_write32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_STATUS,
                            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
            hw_mode = HW_MODE_VIRTIO_GPU;
            sel4_dbg_puts("[framebuffer_pd] HW_DIRECT: virtio-gpu detected (QEMU path)\n");
            return;
        }
        sel4_dbg_puts("[framebuffer_pd] HW_DIRECT: virtio-gpu not at MMIO, trying gpu_sched\n");
    }

    /* No virtio-gpu detected — assume real GPU via gpu_sched (Sparky GB10/NUC) */
    hw_mode = HW_MODE_GPU_SCHED;
    sel4_dbg_puts("[framebuffer_pd] HW_DIRECT: gpu_sched path active (Sparky/NUC)\n");
}

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

/* ─── EventBus publishing ────────────────────────────────────────────────── */

static void publish_frame_ready(uint32_t handle, uint32_t frame_seq,
                                uint32_t backend)
{
    if (!eventbus_connected)
        return;

    rep_u32(rep, 0, (uint64_t)MSG_EVENT_PUBLISH);
    rep_u32(rep, 4, (uint64_t)EVENT_FB_FRAME_READY);
    rep_u32(rep, 8, (uint64_t)handle);
    rep_u32(rep, 12, (uint64_t)frame_seq);
    rep_u32(rep, 16, (uint64_t)backend);
    /* E5-S8: ppcall stubbed */
}

/* ─── IPC handlers ───────────────────────────────────────────────────────── */

static void handle_remote_attach(uint32_t ch)
{
    uint32_t handle = (uint32_t)msg_u32(req, 4);

    fb_surface_t *s = surface_find(handle);
    if (!s) {
        rep_u32(rep, 0, FB_ERR_BAD_HANDLE);
        return;
    }
    if (s->backend != FB_BACKEND_REMOTE_API) {
        rep_u32(rep, 0, FB_ERR_BAD_BACKEND);
        return;
    }

    /* Find a free subscriber slot. */
    for (uint32_t i = 0; i < FB_MAX_REMOTE_SUBS; i++) {
        if (!remote_subs[i].active) {
            remote_subs[i].active = true;
            remote_subs[i].ch     = ch;
            remote_subs[i].handle = handle;
            rep_u32(rep, 0, FB_OK);
            return;
        }
    }

    rep_u32(rep, 0, FB_ERR_NO_SLOTS);
}

static void handle_create(void)
{
    if (!fb_shmem_vaddr) {
        rep_u32(rep, 0, FB_ERR_NO_SHMEM);
        rep_u32(rep, 4, FB_HANDLE_INVALID);
        return;
    }

    const struct fb_create_req *req = (const struct fb_create_req *)fb_shmem_vaddr;

    uint32_t bpp = fmt_bpp(req->format);
    if (bpp == 0) {
        rep_u32(rep, 0, FB_ERR_BAD_FORMAT);
        rep_u32(rep, 4, FB_HANDLE_INVALID);
        return;
    }

    if (req->width == 0 || req->height == 0 ||
        req->width > FB_MAX_WIDTH || req->height > FB_MAX_HEIGHT) {
        rep_u32(rep, 0, FB_ERR_BAD_DIMS);
        rep_u32(rep, 4, FB_HANDLE_INVALID);
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
        rep_u32(rep, 0, FB_ERR_NO_SLOTS);
        rep_u32(rep, 4, FB_HANDLE_INVALID);
        return;
    }

    fb_surface_t *s = surface_alloc();
    if (!s) {
        rep_u32(rep, 0, FB_ERR_NO_SLOTS);
        rep_u32(rep, 4, FB_HANDLE_INVALID);
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

    rep_u32(rep, 0, FB_OK);
    rep_u32(rep, 4, s->handle);
}

static void handle_write(void)
{
    if (!fb_shmem_vaddr) {
        rep_u32(rep, 0, FB_ERR_NO_SHMEM);
        return;
    }

    const struct fb_write_req *req = (const struct fb_write_req *)fb_shmem_vaddr;

    fb_surface_t *s = surface_find(req->fb_handle);
    if (!s) {
        rep_u32(rep, 0, FB_ERR_BAD_HANDLE);
        return;
    }

    /* Validate destination rectangle */
    if (req->x + req->w > s->width || req->y + req->h > s->height ||
        req->w == 0 || req->h == 0) {
        rep_u32(rep, 0, FB_ERR_BAD_RECT);
        return;
    }

    /*
     * Pixel data is already at fb_shmem_vaddr + s->shmem_offset (placed there
     * by the caller before sending MSG_FB_WRITE).  No copy is needed: both
     * sides share the same physical memory region via seL4 shared MR.
     * We simply validate bounds above and count the write.
     */
    s->write_count++;
    rep_u32(rep, 0, FB_OK);
}

static void handle_flip(void)
{
    uint32_t handle = (uint32_t)msg_u32(req, 4);

    fb_surface_t *s = surface_find(handle);
    if (!s) {
        rep_u32(rep, 0, FB_ERR_BAD_HANDLE);
        rep_u32(rep, 4, 0);
        return;
    }

    uint32_t result = backend_ops[s->backend].flip(s->handle, s->frame_seq + 1);
    if (result != FB_OK) {
        rep_u32(rep, 0, result);
        rep_u32(rep, 4, s->frame_seq);
        return;
    }

    s->frame_seq++;
    s->flip_count++;

    /*
     * Notify all registered REMOTE_API subscribers for this framebuffer handle
     * via microkit_notify(); they read pixel data directly from fb_shmem.
     */
    if (s->backend == FB_BACKEND_REMOTE_API) {
        for (uint32_t i = 0; i < FB_MAX_REMOTE_SUBS; i++) {
            if (remote_subs[i].active && remote_subs[i].handle == handle)
                sel4_dbg_puts("[E5-S8] notify-stub
"); /* TODO: seL4_Signal(notify_cap_for_remote_subs[i].ch) */
        }
    }

    /*
     * Publish MSG_FB_FRAME_READY to EventBus so subscribers (e.g. cc_pd)
     * learn a new frame is available without polling.
     */
    publish_frame_ready(handle, s->frame_seq, s->backend);

    rep_u32(rep, 0, FB_OK);
    rep_u32(rep, 4, s->frame_seq);
}

static void handle_resize(void)
{
    if (!fb_shmem_vaddr) {
        rep_u32(rep, 0, FB_ERR_NO_SHMEM);
        return;
    }

    const struct fb_resize_req *req = (const struct fb_resize_req *)fb_shmem_vaddr;

    fb_surface_t *s = surface_find(req->fb_handle);
    if (!s) {
        rep_u32(rep, 0, FB_ERR_BAD_HANDLE);
        return;
    }

    uint32_t new_fmt = (req->format != 0) ? req->format : s->format;
    uint32_t bpp     = fmt_bpp(new_fmt);
    if (bpp == 0) {
        rep_u32(rep, 0, FB_ERR_BAD_FORMAT);
        return;
    }

    if (req->width == 0 || req->height == 0 ||
        req->width > FB_MAX_WIDTH || req->height > FB_MAX_HEIGHT) {
        rep_u32(rep, 0, FB_ERR_BAD_DIMS);
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
                rep_u32(rep, 0, FB_ERR_NO_SHMEM);
                return;
            }
            shmem_used = shmem_used - aligned_old + aligned_new;
        } else {
            /* Mid-table surface — cannot resize in place without compaction */
            rep_u32(rep, 0, FB_ERR_NO_SHMEM);
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
    rep->ok            = FB_OK;
    rep->actual_width  = s->width;
    rep->actual_height = s->height;
    rep->stride        = s->stride;
    rep->buf_size      = s->buf_size;
    rep->shmem_offset  = s->shmem_offset;

    rep_u32(rep, 0, FB_OK);
}

static void handle_destroy(void)
{
    uint32_t handle = (uint32_t)msg_u32(req, 4);

    fb_surface_t *s = surface_find(handle);
    if (!s) {
        rep_u32(rep, 0, FB_ERR_BAD_HANDLE);
        return;
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
    rep_u32(rep, 0, FB_OK);
}

static void handle_status(void)
{
    uint32_t handle = (uint32_t)msg_u32(req, 4);

    fb_surface_t *s = surface_find(handle);
    if (!s) {
        rep_u32(rep, 0, FB_ERR_BAD_HANDLE);
        return;
    }

    if (!fb_shmem_vaddr) {
        rep_u32(rep, 0, FB_ERR_NO_SHMEM);
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

    rep_u32(rep, 0, FB_OK);
}

/* ─── Microkit entry points ──────────────────────────────────────────────── */

static void framebuffer_pd_pd_init(void)
{
    sel4_dbg_puts("[framebuffer_pd] starting — agentOS virtual framebuffer service\n");

    for (uint32_t i = 0; i < FB_MAX_SURFACES; i++)
        surfaces[i].in_use = false;
    shmem_used = 0;

    for (uint32_t i = 0; i < FB_MAX_REMOTE_SUBS; i++)
        remote_subs[i].active = false;

    /* Detect HW_DIRECT backend: virtio-gpu (QEMU) or gpu_sched (Sparky/NUC) */
    probe_hw_direct();

    /*
     * EventBus publishing is enabled only when the system manifest wires
     * CH_FB_EVENTBUS.  The manifest sets fb_eventbus_wired_flag via
     * setvar_vaddr to a non-zero address; otherwise the BSS default of 0
     * keeps publishing disabled (prevents ppcall faults on unwired channels).
     */
    eventbus_connected = (fb_eventbus_wired_flag != 0);

    if (!fb_shmem_vaddr)
        sel4_dbg_puts("[framebuffer_pd] WARNING: fb_shmem_vaddr not mapped "
                          "(NULL backend only)\n");

    if (eventbus_connected)
        sel4_dbg_puts("[framebuffer_pd] ready (EventBus wired)\n");
    else
        sel4_dbg_puts("[framebuffer_pd] ready\n");
}

static void framebuffer_pd_pd_notified(uint32_t ch)
{
    (void)ch;
    /* No async notifications expected in Phase 4a core */
}

static uint32_t framebuffer_pd_h_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)ctx;

    uint32_t op = (uint32_t)msg_u32(req, 0);

    switch (op) {
    case MSG_FB_CREATE:             handle_create();           break;
    case MSG_FB_WRITE:              handle_write();            break;
    case MSG_FB_FLIP:               handle_flip();             break;
    case MSG_FB_RESIZE:             handle_resize();           break;
    case MSG_FB_DESTROY:            handle_destroy();          break;
    case MSG_FB_STATUS:             handle_status();           break;
    case MSG_CC_ATTACH_FRAMEBUFFER: handle_remote_attach(ch);  break;
    default:
        sel4_dbg_puts("[framebuffer_pd] unknown opcode\n");
        rep_u32(rep, 0, FB_ERR_BAD_HANDLE);
        break;
    }

    rep->length = 24;
        return SEL4_ERR_OK;
}

/* ── E5-S8: Entry point ─────────────────────────────────────────────────── */
void framebuffer_pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    framebuffer_pd_pd_init();
    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    /* Dispatch all opcodes through the generic handler */
    sel4_server_register(&srv, SEL4_SERVER_OPCODE_ANY, framebuffer_pd_h_dispatch, (void *)0);
    sel4_server_run(&srv);
}
