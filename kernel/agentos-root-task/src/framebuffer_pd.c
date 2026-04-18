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
#include "contracts/framebuffer_contract.h"
#include "contracts/cc_contract.h"
#include "gpu_sched.h"

/* ─── Channel assignments ────────────────────────────────────────────────── */

#define CH_FB_EVENTBUS      1u   /* optional: framebuffer_pd → EventBus (PPC) */
#define CH_FB_GPU_SCHED     2u   /* framebuffer_pd → gpu_sched (Sparky real-GPU path) */
#define CH_FB_REMOTE_BASE   2u   /* remote subscriber channels: 2..5 */

/* ─── Limits ─────────────────────────────────────────────────────────────── */

#define FB_MAX_CLIENTS      8u
#define FB_MAX_REMOTE_SUBS  4u

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

/* ─── Virtual framebuffer slot ───────────────────────────────────────────── */

typedef struct {
    bool             active;
    microkit_channel owner;       /* channel that issued MSG_FB_CREATE */
    uint32_t         width;
    uint32_t         height;
    uint32_t         format;      /* FB_FMT_* */
    uint32_t         backend;     /* FB_BACKEND_* */
    uint32_t         frame_seq;   /* incremented on each successful FLIP */
} fb_slot_t;

static fb_slot_t   slots[FB_MAX_CLIENTS];
static bool        eventbus_connected = false;

/* ─── Remote subscriber table ────────────────────────────────────────────── */

typedef struct {
    bool             active;
    microkit_channel ch;      /* calling channel that registered this subscriber */
    uint32_t         handle;  /* framebuffer handle the subscriber is attached to */
} fb_remote_sub_t;

static fb_remote_sub_t remote_subs[FB_MAX_REMOTE_SUBS];

/* ─── Backend dispatch table ─────────────────────────────────────────────── */

typedef struct {
    fb_error_t (*on_write)(uint32_t slot, const struct fb_write_req *req);
    fb_error_t (*on_flip)(uint32_t slot, uint32_t *frame_seq_out);
    fb_error_t (*on_resize)(uint32_t slot, uint32_t new_w, uint32_t new_h);
    void       (*on_destroy)(uint32_t slot);
} fb_backend_ops_t;

/* ─── NULL backend ───────────────────────────────────────────────────────── */

static fb_error_t null_write(uint32_t slot, const struct fb_write_req *req)
{
    (void)slot;
    (void)req;
    return FB_OK;
}

static fb_error_t null_flip(uint32_t slot, uint32_t *frame_seq_out)
{
    slots[slot].frame_seq++;
    *frame_seq_out = slots[slot].frame_seq;
    return FB_OK;
}

static fb_error_t null_resize(uint32_t slot, uint32_t new_w, uint32_t new_h)
{
    slots[slot].width  = new_w;
    slots[slot].height = new_h;
    return FB_OK;
}

static void null_destroy(uint32_t slot) { (void)slot; }

static const fb_backend_ops_t null_backend_ops = {
    .on_write   = null_write,
    .on_flip    = null_flip,
    .on_resize  = null_resize,
    .on_destroy = null_destroy,
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
            microkit_dbg_puts("[framebuffer_pd] HW_DIRECT: virtio-gpu detected (QEMU path)\n");
            return;
        }
        microkit_dbg_puts("[framebuffer_pd] HW_DIRECT: virtio-gpu not at MMIO, trying gpu_sched\n");
    }

    /* No virtio-gpu detected — assume real GPU via gpu_sched (Sparky GB10/NUC) */
    hw_mode = HW_MODE_GPU_SCHED;
    microkit_dbg_puts("[framebuffer_pd] HW_DIRECT: gpu_sched path active (Sparky/NUC)\n");
}

static fb_error_t hw_direct_write(uint32_t slot, const struct fb_write_req *req)
{
    (void)slot;
    (void)req;
    /* Pixel data is already in fb_shmem written by the caller.
     * No staging needed: the full frame is submitted on FLIP. */
    return FB_OK;
}

static fb_error_t hw_direct_flip(uint32_t slot, uint32_t *frame_seq_out)
{
    fb_slot_t *s = &slots[slot];

    if (hw_mode == HW_MODE_VIRTIO_GPU) {
        /*
         * QEMU path: DMA fb_shmem region to the virtio-gpu controlq (queue 0).
         *
         * A full production pass will allocate a virtio descriptor table and
         * populate desc.addr = phys(fb_shmem), desc.len = frame_bytes, then
         * update the available ring and kick the queue.  The descriptor table
         * allocation is deferred to the gpu_shmem integration phase (the same
         * deferral pattern used in gpu_sched.c OP_GPU_SUBMIT_CMD).
         *
         * We issue the memory barrier and queue kick now so the QEMU backend
         * processes any descriptors that a future integration pass has placed.
         */
        __asm__ volatile("" ::: "memory");  /* ensure fb_shmem writes visible */
        fb_mmio_write32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_QUEUE_NOTIFY, 0u);

        s->frame_seq++;
        *frame_seq_out = s->frame_seq;
        return FB_OK;
    }

    if (hw_mode == HW_MODE_GPU_SCHED) {
        /*
         * Sparky GB10 / NUC path: submit the frame as a GPU blit job via
         * gpu_sched's OP_GPU_SUBMIT_CMD IPC.
         *
         * GPU_INTEGRATION_POINT: when gpu_shmem is fully wired, replace the
         * cmd_offset / cmd_len pair with the appropriate gpu_shmem slot
         * coordinates for a virtio-gpu RESOURCE_FLUSH blit command.
         *
         * For now cmd_offset=0 (start of fb_shmem) and cmd_len = full frame.
         */
        uint32_t frame_bytes = s->width * s->height * 4u; /* XRGB8888 / ARGB8888 */
        microkit_mr_set(1, 0u);           /* gpu_shmem slot 0: display output plane */
        microkit_mr_set(2, 0u);           /* cmd_offset: start of fb_shmem */
        microkit_mr_set(3, frame_bytes);  /* cmd_len: one full frame */
        microkit_ppcall(CH_FB_GPU_SCHED,
                        microkit_msginfo_new(OP_GPU_SUBMIT_CMD, 4));

        if (microkit_mr_get(0) != 0u) {
            microkit_dbg_puts("[framebuffer_pd] HW_DIRECT flip: gpu_sched error\n");
            return FB_ERR_BACKEND_UNAVAIL;
        }

        s->frame_seq++;
        *frame_seq_out = s->frame_seq;
        return FB_OK;
    }

    return FB_ERR_BACKEND_UNAVAIL;
}

static fb_error_t hw_direct_resize(uint32_t slot, uint32_t new_w, uint32_t new_h)
{
    /* Update dimensions; the next FLIP will use the new frame size. */
    slots[slot].width  = new_w;
    slots[slot].height = new_h;
    return FB_OK;
}

static void hw_direct_destroy(uint32_t slot)
{
    (void)slot;
    /* GPU_INTEGRATION_POINT: release gpu_shmem slot and virtio-gpu resource. */
}

static const fb_backend_ops_t hw_direct_ops = {
    .on_write   = hw_direct_write,
    .on_flip    = hw_direct_flip,
    .on_resize  = hw_direct_resize,
    .on_destroy = hw_direct_destroy,
};

/* ─── REMOTE_API stub (Phase 4a-remote, ag-tz2) ─────────────────────────── */

static fb_error_t remote_stub_write(uint32_t slot, const struct fb_write_req *req)
{
    (void)slot; (void)req;
    return FB_ERR_BACKEND_UNAVAIL;
}

static fb_error_t remote_stub_flip(uint32_t slot, uint32_t *out)
{
    (void)slot; (void)out;
    return FB_ERR_BACKEND_UNAVAIL;
}

static fb_error_t remote_stub_resize(uint32_t slot, uint32_t w, uint32_t h)
{
    (void)slot; (void)w; (void)h;
    return FB_ERR_BACKEND_UNAVAIL;
}

static void stub_destroy(uint32_t slot) { (void)slot; }

static const fb_backend_ops_t hw_direct_ops = {
    .on_write   = stub_write,
    .on_flip    = stub_flip,
    .on_resize  = stub_resize,
    .on_destroy = stub_destroy,
};

/* ─── REMOTE_API backend (Phase 4a-remote, ag-tz2) ──────────────────────── */

/*
 * Write: pixel data is already in fb_shmem (written by the caller before
 * MSG_FB_WRITE).  No copy needed — subscribers read directly from shmem.
 */
static fb_error_t remote_write(uint32_t slot, const struct fb_write_req *req)
{
    (void)slot; (void)req;
    return FB_OK;
}

/*
 * Flip: commit the frame.  Notify all registered subscribers for this
 * framebuffer handle via microkit_notify(); they read from fb_shmem.
 */
static fb_error_t remote_flip(uint32_t slot, uint32_t *frame_seq_out)
{
    slots[slot].frame_seq++;
    *frame_seq_out = slots[slot].frame_seq;

    for (uint32_t i = 0; i < FB_MAX_REMOTE_SUBS; i++) {
        if (remote_subs[i].active && remote_subs[i].handle == slot)
            microkit_notify(remote_subs[i].ch);
    }
    return FB_OK;
}

static fb_error_t remote_resize(uint32_t slot, uint32_t new_w, uint32_t new_h)
{
    slots[slot].width  = new_w;
    slots[slot].height = new_h;
    return FB_OK;
}

/* On destroy, detach all subscribers for this framebuffer. */
static void remote_destroy(uint32_t slot)
{
    for (uint32_t i = 0; i < FB_MAX_REMOTE_SUBS; i++) {
        if (remote_subs[i].active && remote_subs[i].handle == slot)
            remote_subs[i].active = false;
    }
}

static const fb_backend_ops_t remote_api_ops = {
    .on_write   = remote_write,
    .on_flip    = remote_flip,
    .on_resize  = remote_resize,
    .on_destroy = remote_destroy,
};

/* ─── Dispatch table indexed by FB_BACKEND_* ─────────────────────────────── */

static const fb_backend_ops_t *backend_table[3] = {
    [FB_BACKEND_NULL]       = &null_backend_ops,
    [FB_BACKEND_HW_DIRECT]  = &hw_direct_ops,
    [FB_BACKEND_REMOTE_API] = &remote_api_ops,
};

/* ─── EventBus publishing ────────────────────────────────────────────────── */

static void publish_frame_ready(uint32_t handle, uint32_t frame_seq,
                                uint32_t backend)
{
    if (!eventbus_connected)
        return;

    microkit_mr_set(0, (uint64_t)MSG_EVENT_PUBLISH);
    microkit_mr_set(1, (uint64_t)EVENT_FB_FRAME_READY);
    microkit_mr_set(2, (uint64_t)handle);
    microkit_mr_set(3, (uint64_t)frame_seq);
    microkit_mr_set(4, (uint64_t)backend);
    microkit_ppcall(CH_FB_EVENTBUS, microkit_msginfo_new(MSG_EVENT_PUBLISH, 5));
}

/* ─── Helpers ────────────────────────────────────────────────────────────── */

static int alloc_slot(void)
{
    for (int i = 0; i < (int)FB_MAX_CLIENTS; i++) {
        if (!slots[i].active)
            return i;
    }
    return -1;
}

static bool valid_format(uint32_t fmt)
{
    return fmt == FB_FMT_XRGB8888 ||
           fmt == FB_FMT_ARGB8888 ||
           fmt == FB_FMT_RGB565;
}

static bool valid_backend(uint32_t b)
{
    return b <= FB_BACKEND_REMOTE_API;
}

static bool slot_owned(uint32_t handle, microkit_channel ch)
{
    return handle < FB_MAX_CLIENTS &&
           slots[handle].active &&
           slots[handle].owner == ch;
}

/* ─── IPC handlers ───────────────────────────────────────────────────────── */

static void handle_remote_attach(microkit_channel ch)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);

    if (handle >= FB_MAX_CLIENTS || !slots[handle].active) {
        microkit_mr_set(0, FB_ERR_BAD_HANDLE);
        return;
    }
    if (slots[handle].backend != FB_BACKEND_REMOTE_API) {
        microkit_mr_set(0, FB_ERR_BAD_BACKEND);
        return;
    }

    /* Find a free subscriber slot. */
    for (uint32_t i = 0; i < FB_MAX_REMOTE_SUBS; i++) {
        if (!remote_subs[i].active) {
            remote_subs[i].active = true;
            remote_subs[i].ch     = ch;
            remote_subs[i].handle = handle;
            microkit_mr_set(0, FB_OK);
            return;
        }
    }

    microkit_mr_set(0, FB_ERR_NO_SLOTS);
}

static void handle_create(microkit_channel ch)
{
    const struct fb_create_req *req =
        (const struct fb_create_req *)fb_shmem_vaddr;

    if (!req->width || !req->height) {
        microkit_mr_set(0, FB_ERR_BAD_GEOMETRY);
        microkit_mr_set(1, 0xFFFFFFFFu);
        return;
    }
    if (!valid_format(req->format)) {
        microkit_mr_set(0, FB_ERR_BAD_FORMAT);
        microkit_mr_set(1, 0xFFFFFFFFu);
        return;
    }
    if (!valid_backend(req->backend)) {
        microkit_mr_set(0, FB_ERR_BAD_BACKEND);
        microkit_mr_set(1, 0xFFFFFFFFu);
        return;
    }

    int s = alloc_slot();
    if (s < 0) {
        microkit_mr_set(0, FB_ERR_NO_SLOTS);
        microkit_mr_set(1, 0xFFFFFFFFu);
        return;
    }

    slots[s].active    = true;
    slots[s].owner     = ch;
    slots[s].width     = req->width;
    slots[s].height    = req->height;
    slots[s].format    = req->format;
    slots[s].backend   = req->backend;
    slots[s].frame_seq = 0;

    microkit_mr_set(0, FB_OK);
    microkit_mr_set(1, (uint32_t)s);
}

static void handle_write(microkit_channel ch)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);

    if (!slot_owned(handle, ch)) {
        microkit_mr_set(0, FB_ERR_BAD_HANDLE);
        return;
    }

    const struct fb_write_req *req =
        (const struct fb_write_req *)fb_shmem_vaddr;

    /* Validate dirty rect fits within the framebuffer */
    if ((uint64_t)req->x + req->w > slots[handle].width ||
        (uint64_t)req->y + req->h > slots[handle].height) {
        microkit_mr_set(0, FB_ERR_OUT_OF_BOUNDS);
        return;
    }

    const fb_backend_ops_t *ops = backend_table[slots[handle].backend];
    fb_error_t err = ops->on_write(handle, req);
    microkit_mr_set(0, (uint32_t)err);
}

static void handle_flip(microkit_channel ch)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);

    if (!slot_owned(handle, ch)) {
        microkit_mr_set(0, FB_ERR_BAD_HANDLE);
        microkit_mr_set(1, 0);
        return;
    }

    uint32_t frame_seq = 0;
    const fb_backend_ops_t *ops = backend_table[slots[handle].backend];
    fb_error_t err = ops->on_flip(handle, &frame_seq);

    if (err == FB_OK) {
        publish_frame_ready(handle, frame_seq, slots[handle].backend);
        microkit_mr_set(0, FB_OK);
        microkit_mr_set(1, frame_seq);
    } else {
        microkit_mr_set(0, (uint32_t)err);
        microkit_mr_set(1, 0);
    }
}

static void handle_resize(microkit_channel ch)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);

    if (!slot_owned(handle, ch)) {
        microkit_mr_set(0, FB_ERR_BAD_HANDLE);
        return;
    }

    const struct fb_resize_req *req =
        (const struct fb_resize_req *)fb_shmem_vaddr;

    if (!req->new_width || !req->new_height) {
        microkit_mr_set(0, FB_ERR_BAD_GEOMETRY);
        return;
    }

    const fb_backend_ops_t *ops = backend_table[slots[handle].backend];
    fb_error_t err = ops->on_resize(handle, req->new_width, req->new_height);
    microkit_mr_set(0, (uint32_t)err);
}

static void handle_destroy(microkit_channel ch)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);

    if (!slot_owned(handle, ch)) {
        microkit_mr_set(0, FB_ERR_BAD_HANDLE);
        return;
    }

    const fb_backend_ops_t *ops = backend_table[slots[handle].backend];
    ops->on_destroy(handle);
    slots[handle].active = false;

    microkit_mr_set(0, FB_OK);
}

/* ─── Microkit entry points ──────────────────────────────────────────────── */

void init(void)
{
    microkit_dbg_puts("[framebuffer_pd] starting — agentOS virtual framebuffer service\n");

    for (uint32_t i = 0; i < FB_MAX_CLIENTS; i++)
        slots[i].active = false;

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

    if (eventbus_connected)
        microkit_dbg_puts("[framebuffer_pd] ready (EventBus wired)\n");
    else
        microkit_dbg_puts("[framebuffer_pd] ready\n");
}

void notified(microkit_channel ch)
{
    (void)ch;
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)msginfo;

    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {
    case MSG_FB_CREATE:             handle_create(ch);        break;
    case MSG_FB_WRITE:              handle_write(ch);         break;
    case MSG_FB_FLIP:               handle_flip(ch);          break;
    case MSG_FB_RESIZE:             handle_resize(ch);        break;
    case MSG_FB_DESTROY:            handle_destroy(ch);       break;
    case MSG_CC_ATTACH_FRAMEBUFFER: handle_remote_attach(ch); break;
    default:
        microkit_dbg_puts("[framebuffer_pd] unknown opcode\n");
        microkit_mr_set(0, FB_ERR_BAD_HANDLE);
        break;
    }

    return microkit_msginfo_new(0, 2);
}
