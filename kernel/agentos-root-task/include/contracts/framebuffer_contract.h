/*
 * Framebuffer PD IPC Contract — Phase 4a-core
 *
 * Defines the virtual framebuffer protocol between guest OSes (via their VMM
 * PDs) and framebuffer_pd.  Each guest that wants display output calls
 * MSG_FB_CREATE to allocate a virtual framebuffer; guests that do not call
 * MSG_FB_CREATE receive no framebuffer capability.
 *
 * Backend dispatch (selected at CREATE time):
 *   FB_BACKEND_NULL       — frames discarded; for headless guests
 *   FB_BACKEND_HW_DIRECT  — virtio-gpu / gpu_sched (Phase 4a-hw, ag-1es)
 *   FB_BACKEND_REMOTE_API — display server relay (Phase 4a-remote, ag-tz2)
 *
 * Protocol:
 *   MSG_FB_CREATE   allocate virtual framebuffer (width, height, format)
 *   MSG_FB_WRITE    blit pixels from shmem into the virtual framebuffer
 *   MSG_FB_FLIP     commit the current frame → triggers backend dispatch
 *   MSG_FB_RESIZE   resize virtual framebuffer dimensions
 *   MSG_FB_DESTROY  release virtual framebuffer and cap token
 *
 *   MSG_FB_FRAME_READY is not a request; it is an EventBus event kind
 *   published by framebuffer_pd after every successful FLIP.
 *   Subscribers receive EVENT_FB_FRAME_READY (0x40) notifications.
 *
 * Guest binding:
 *   MSG_GUEST_BIND_DEVICE(GUEST_DEV_FB) carries the handle returned by
 *   MSG_FB_CREATE as dev_handle.  The root-task validates the handle and
 *   returns a cap_token stored in guest_capabilities_t.fb_token.
 *
 * Channel: CH_FB_PD (see agentos.h)
 * Opcodes: MSG_FB_* (see agentos.h)
 *
 * Invariants:
 *   - MSG_FB_CREATE returns a handle; all subsequent calls reference it.
 *   - Handle ownership is scoped to the calling channel (microkit_channel).
 *   - MSG_FB_WRITE does not commit the frame; only MSG_FB_FLIP dispatches.
 *   - MSG_FB_FLIP on the NULL backend increments frame_seq and returns ok.
 *   - MSG_FB_DESTROY revokes the cap_token (if bound via guest_contract).
 */

#pragma once
#include "../agentos.h"

/* ─── Channel reference ──────────────────────────────────────────────────── */

#define FB_PD_CH_CONTROLLER  CH_FB_PD

/* ─── Backend selection constants ────────────────────────────────────────── */

#define FB_BACKEND_NULL        0u  /* headless — frames discarded */
#define FB_BACKEND_HW_DIRECT   1u  /* virtio-gpu / gpu_sched (Phase 4a-hw) */
#define FB_BACKEND_REMOTE_API  2u  /* display relay (Phase 4a-remote) */

/* ─── Pixel format constants ─────────────────────────────────────────────── */

#define FB_FMT_XRGB8888  0x00u
#define FB_FMT_ARGB8888  0x01u
#define FB_FMT_RGB565    0x02u

/* ─── MSG_FB_CREATE ──────────────────────────────────────────────────────── */

/*
 * Allocate a virtual framebuffer.  Pixel data is later delivered via
 * MSG_FB_WRITE into the shared fb_shmem region, then committed by
 * MSG_FB_FLIP.  Returns an opaque handle used in all subsequent calls.
 */

struct fb_create_req {
    uint32_t width;          /* framebuffer width in pixels */
    uint32_t height;         /* framebuffer height in pixels */
    uint32_t format;         /* FB_FMT_* */
    uint32_t backend;        /* FB_BACKEND_* */
    uint32_t _reserved[4];
};

struct fb_create_reply {
    uint32_t ok;             /* FB_OK on success */
    uint32_t handle;         /* opaque handle; 0xFFFFFFFF = error */
};

/* ─── MSG_FB_WRITE ───────────────────────────────────────────────────────── */

/*
 * Blit pixel data from the shared fb_shmem region into the virtual
 * framebuffer.  The caller writes pixel data to fb_shmem_vaddr before
 * calling MSG_FB_WRITE.  shmem_offset is the byte offset within fb_shmem.
 * The dirty rectangle (x, y, w, h) describes which portion of the
 * framebuffer the pixel data covers; framebuffer_pd validates the bounds.
 */

struct fb_write_req {
    uint32_t x;              /* left edge of dirty rect (pixels) */
    uint32_t y;              /* top edge of dirty rect (pixels) */
    uint32_t w;              /* width of dirty rect (pixels) */
    uint32_t h;              /* height of dirty rect (pixels) */
    uint32_t shmem_offset;   /* byte offset into fb_shmem of pixel data */
    uint32_t stride;         /* bytes per row in the source pixel data */
    uint32_t _reserved[2];
};

struct fb_write_reply {
    uint32_t ok;             /* FB_OK on success */
};

/* ─── MSG_FB_FLIP ────────────────────────────────────────────────────────── */

/*
 * Commit the current frame and dispatch to the selected backend.
 * On success, frame_seq is a monotonically increasing counter that
 * identifies this frame to EventBus subscribers via EVENT_FB_FRAME_READY.
 *
 * NULL backend: frame is discarded; frame_seq is still incremented and
 * EVENT_FB_FRAME_READY is published so headless consumers remain unblocked.
 */

struct fb_flip_reply {
    uint32_t ok;             /* FB_OK on success */
    uint32_t frame_seq;      /* frame sequence number */
};

/* ─── MSG_FB_RESIZE ──────────────────────────────────────────────────────── */

/*
 * Resize the virtual framebuffer.  Any pixel data written before RESIZE
 * is discarded.  The next MSG_FB_WRITE must target the new dimensions.
 */

struct fb_resize_req {
    uint32_t new_width;      /* new framebuffer width (pixels) */
    uint32_t new_height;     /* new framebuffer height (pixels) */
    uint32_t _reserved[2];
};

struct fb_resize_reply {
    uint32_t ok;             /* FB_OK on success */
};

/* ─── MSG_FB_DESTROY ─────────────────────────────────────────────────────── */

/*
 * Release the virtual framebuffer.  Any guest cap_token bound to this
 * handle (via MSG_GUEST_BIND_DEVICE) is revoked.  The handle is invalid
 * after this call.
 */

struct fb_destroy_reply {
    uint32_t ok;             /* FB_OK on success */
};

/* ─── MSG_FB_FRAME_READY (EventBus event) ────────────────────────────────── */

/*
 * Published to EventBus by framebuffer_pd after every successful MSG_FB_FLIP.
 * Event kind = EVENT_FB_FRAME_READY (0x40).
 * Subscribers read this from the EventBus ring; it is NOT an IPC request.
 */

typedef struct __attribute__((packed)) {
    uint32_t handle;         /* framebuffer handle that was flipped */
    uint32_t frame_seq;      /* frame sequence number */
    uint32_t backend;        /* FB_BACKEND_* that processed the flip */
    uint32_t _reserved;
} fb_frame_ready_event_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

typedef enum {
    FB_OK                 = 0,
    FB_ERR_BAD_HANDLE     = 1,   /* handle unknown or not owned by caller */
    FB_ERR_NO_SLOTS       = 2,   /* all virtual framebuffer slots occupied */
    FB_ERR_BAD_GEOMETRY   = 3,   /* width or height is zero */
    FB_ERR_BAD_FORMAT     = 4,   /* format not one of FB_FMT_* */
    FB_ERR_BAD_BACKEND    = 5,   /* backend not one of FB_BACKEND_* */
    FB_ERR_OUT_OF_BOUNDS  = 6,   /* write rect exceeds framebuffer dimensions */
    FB_ERR_BACKEND_UNAVAIL = 7,  /* HW_DIRECT / REMOTE_API not yet implemented */
} fb_error_t;
