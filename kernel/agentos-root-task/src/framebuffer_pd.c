/*
 * framebuffer_pd.c — agentOS Virtual Framebuffer Protection Domain
 *
 * Manages virtual framebuffers for guest OSes via a backend dispatch model.
 * Each guest selects a backend at MSG_FB_CREATE time:
 *
 *   FB_BACKEND_NULL       — frames discarded; for headless guests (this file)
 *   FB_BACKEND_HW_DIRECT  — virtio-gpu / gpu_sched (Phase 4a-hw, ag-1es)
 *   FB_BACKEND_REMOTE_API — display server relay (Phase 4a-remote, ag-tz2)
 *
 * IPC protocol (opcode in MR0):
 *   MSG_FB_CREATE   fb_create_req in shmem → MR0=ok MR1=handle
 *   MSG_FB_WRITE    MR1=handle; fb_write_req in shmem → MR0=ok
 *   MSG_FB_FLIP     MR1=handle → MR0=ok MR1=frame_seq
 *   MSG_FB_RESIZE   MR1=handle; fb_resize_req in shmem → MR0=ok
 *   MSG_FB_DESTROY  MR1=handle → MR0=ok
 *
 * On MSG_FB_FLIP the NULL backend discards the frame, increments frame_seq,
 * and publishes EVENT_FB_FRAME_READY to EventBus (channel CH_FB_EVENTBUS).
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
 * Channel: CH_FB_PD (see agentos.h) — inbound PPC from callers
 * Optional channel 1: CH_FB_EVENTBUS — outbound to EventBus for FRAME_READY
 * Priority: 175  (below serial; above idle workers)
 * Mode: passive  (woken by PPC from callers)
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/framebuffer_contract.h"

/* ─── Channel assignments ────────────────────────────────────────────────── */

#define CH_FB_EVENTBUS  1u   /* optional: framebuffer_pd → EventBus (PPC) */

/* ─── Limits ─────────────────────────────────────────────────────────────── */

#define FB_MAX_CLIENTS  8u

/* ─── Shmem (Microkit setvar_vaddr) ─────────────────────────────────────── */

uintptr_t fb_shmem_vaddr;

/* Declared for log_drain_write(); falls back to microkit_dbg_puts if unmapped */
uintptr_t log_drain_rings_vaddr;

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

/* ─── HW_DIRECT stub (Phase 4a-hw, ag-1es) ──────────────────────────────── */

static fb_error_t stub_write(uint32_t slot, const struct fb_write_req *req)
{
    (void)slot; (void)req;
    return FB_ERR_BACKEND_UNAVAIL;
}

static fb_error_t stub_flip(uint32_t slot, uint32_t *out)
{
    (void)slot; (void)out;
    return FB_ERR_BACKEND_UNAVAIL;
}

static fb_error_t stub_resize(uint32_t slot, uint32_t w, uint32_t h)
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

/* ─── REMOTE_API stub (Phase 4a-remote, ag-tz2) ─────────────────────────── */

static const fb_backend_ops_t remote_api_ops = {
    .on_write   = stub_write,
    .on_flip    = stub_flip,
    .on_resize  = stub_resize,
    .on_destroy = stub_destroy,
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

    /*
     * EventBus publishing is disabled until Phase 4a-remote (ag-tz2) adds
     * the system manifest channel wiring for CH_FB_EVENTBUS.  Calling
     * microkit_ppcall on an unwired channel faults, so we cannot probe it
     * safely here.  The Phase 4a-remote bead sets eventbus_connected = true
     * after confirming the channel is wired in the manifest.
     */
    eventbus_connected = false;

    microkit_dbg_puts("[framebuffer_pd] ready (NULL backend active; EventBus pending manifest wiring)\n");
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
    case MSG_FB_CREATE:  handle_create(ch);  break;
    case MSG_FB_WRITE:   handle_write(ch);   break;
    case MSG_FB_FLIP:    handle_flip(ch);    break;
    case MSG_FB_RESIZE:  handle_resize(ch);  break;
    case MSG_FB_DESTROY: handle_destroy(ch); break;
    default:
        microkit_dbg_puts("[framebuffer_pd] unknown opcode\n");
        microkit_mr_set(0, FB_ERR_BAD_HANDLE);
        break;
    }

    return microkit_msginfo_new(0, 2);
}
