/*
 * Framebuffer PD IPC Contract — Phase 4a
 *
 * Virtual framebuffer API for guest OSes and VibeOS.  Graphics is opt-in:
 * guests that never call MSG_FB_CREATE receive no framebuffer capability.
 *
 * Backend dispatch (selected at CREATE time):
 *   FB_BACKEND_NULL       — frames discarded; for headless guests and CI
 *   FB_BACKEND_HW_DIRECT  — virtio-gpu / gpu_sched (Phase 4a-hw, ag-1es)
 *   FB_BACKEND_REMOTE_API — display server relay (Phase 4a-remote, ag-tz2)
 *
 * Channel: CH_FB_PD (see agentos.h)
 * Opcodes: MSG_FB_* (see agentos.h)
 *
 * Lifecycle:
 *   MSG_FB_CREATE  → fb_handle
 *   MSG_FB_WRITE   → blit pixels via shared memory
 *   MSG_FB_FLIP    → commit frame (backend dispatch)
 *   MSG_FB_RESIZE  → change dimensions
 *   MSG_FB_STATUS  → query current geometry and statistics
 *   MSG_FB_DESTROY → release handle and region
 *
 * MSG_FB_FRAME_READY is published to EventBus after each successful FLIP.
 * Subscribers (e.g. cc_pd display relay) consume it without polling.
 *
 * Guest integration: pass fb_handle as dev_handle in MSG_GUEST_BIND_DEVICE
 * with dev_type = GUEST_DEV_FB.  The root-task issues a cap_token; all
 * subsequent IPC from the guest VMM must present that token.
 *
 * Invariants:
 *   - MSG_FB_CREATE returns fb_handle; FB_HANDLE_INVALID (0) means error.
 *   - Handle ownership is scoped to the calling channel (microkit_channel).
 *   - MSG_FB_WRITE does not commit the frame; only MSG_FB_FLIP dispatches.
 *   - NULL backend: FLIP is accepted and frame_seq incremented; pixel data
 *     is silently discarded.  Suitable for headless guests and CI.
 *   - MSG_FB_DESTROY revokes the cap_token (if bound via guest_contract).
 */

#pragma once
#include "../agentos.h"

/* ─── Handle sentinel ────────────────────────────────────────────────────── */

#define FB_HANDLE_INVALID  0u

/* ─── Channel reference ──────────────────────────────────────────────────── */

#define FB_PD_CH_CONTROLLER  CH_FB_PD

/* ─── Backend selection ──────────────────────────────────────────────────── */

#define FB_BACKEND_NULL        0u  /* headless — frames discarded */
#define FB_BACKEND_HW_DIRECT   1u  /* virtio-gpu / gpu_sched (Phase 4a-hw) */
#define FB_BACKEND_REMOTE_API  2u  /* display relay (Phase 4a-remote) */

typedef enum {
    FB_BACKEND_NULL_T       = 0,
    FB_BACKEND_HW_DIRECT_T  = 1,
    FB_BACKEND_REMOTE_API_T = 2,
} fb_backend_t;

/* ─── Pixel format constants ─────────────────────────────────────────────── */

#define FB_FMT_XRGB8888  0x00u
#define FB_FMT_ARGB8888  0x01u
#define FB_FMT_RGB565    0x02u

/* ─── MSG_FB_CREATE ──────────────────────────────────────────────────────── */

struct fb_create_req {
    uint32_t width;           /* framebuffer width in pixels */
    uint32_t height;          /* framebuffer height in pixels */
    uint32_t format;          /* FB_FMT_* */
    uint32_t backend;         /* fb_backend_t — caller may request; PD may downgrade */
};

struct fb_create_reply {
    uint32_t ok;              /* FB_OK on success */
    uint32_t fb_handle;       /* opaque handle; FB_HANDLE_INVALID = error */
    uint32_t actual_backend;  /* fb_backend_t actually assigned */
    uint32_t stride;          /* bytes per row */
    uint32_t buf_size;        /* total size of pixel region in fb_shmem */
    uint32_t shmem_offset;    /* byte offset into fb_shmem for this handle's region */
};

/* ─── MSG_FB_WRITE ───────────────────────────────────────────────────────── */

/*
 * Caller places pixel data at fb_shmem_vaddr + shmem_offset (from CREATE reply)
 * then sends MSG_FB_WRITE.  The PD validates bounds; no copy occurs — the caller
 * and PD share the same physical memory region.
 */
struct fb_write_req {
    uint32_t fb_handle;
    uint32_t x;               /* destination rectangle, pixels */
    uint32_t y;
    uint32_t w;
    uint32_t h;
    uint32_t src_stride;      /* bytes per row of caller's source rectangle */
};

struct fb_write_reply {
    uint32_t ok;              /* FB_OK on success */
};

/* ─── MSG_FB_FLIP ────────────────────────────────────────────────────────── */

struct fb_flip_req {
    uint32_t fb_handle;
};

struct fb_flip_reply {
    uint32_t ok;              /* FB_OK on success */
    uint32_t frame_seq;       /* monotonically increasing frame counter */
};

/* ─── MSG_FB_RESIZE ──────────────────────────────────────────────────────── */

struct fb_resize_req {
    uint32_t fb_handle;
    uint32_t width;           /* new framebuffer width (pixels) */
    uint32_t height;          /* new framebuffer height (pixels) */
    uint32_t format;          /* FB_FMT_* — 0 = keep current */
};

struct fb_resize_reply {
    uint32_t ok;              /* FB_OK on success */
    uint32_t actual_width;
    uint32_t actual_height;
    uint32_t stride;
    uint32_t buf_size;
    uint32_t shmem_offset;    /* may change if region was relocated */
};

/* ─── MSG_FB_STATUS ──────────────────────────────────────────────────────── */

struct fb_status_req {
    uint32_t fb_handle;
};

struct fb_status_reply {
    uint32_t ok;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t backend;         /* fb_backend_t */
    uint32_t frame_seq;       /* last completed frame */
    uint32_t flip_count;      /* total FLIPs since CREATE */
    uint32_t write_count;     /* total WRITEs since CREATE */
};

/* ─── MSG_FB_DESTROY ─────────────────────────────────────────────────────── */

struct fb_destroy_req {
    uint32_t fb_handle;
};

struct fb_destroy_reply {
    uint32_t ok;              /* FB_OK on success */
};

/* ─── MSG_FB_FRAME_READY (EventBus event) ────────────────────────────────── */

/*
 * Published to EventBus by framebuffer_pd after every successful MSG_FB_FLIP.
 * Event kind = EVENT_FB_FRAME_READY (0x40) / EVT_FB_FRAME_READY (0x5001).
 * Subscribers read this from the EventBus ring; it is NOT an IPC request.
 */
typedef struct __attribute__((packed)) {
    uint32_t fb_handle;       /* framebuffer handle that was flipped */
    uint32_t frame_seq;       /* frame sequence number */
    uint32_t width;
    uint32_t height;
    uint32_t format;          /* FB_FMT_* */
    uint32_t backend;         /* FB_BACKEND_* that processed the flip */
} fb_frame_ready_event_t;

#define EVT_FB_FRAME_READY  0x5001u

/* ─── Error codes ────────────────────────────────────────────────────────── */

typedef enum {
    FB_OK                   = 0,
    FB_ERR_BAD_HANDLE       = 1,   /* handle unknown or not owned by caller */
    FB_ERR_NO_SLOTS         = 2,   /* all virtual framebuffer slots occupied */
    FB_ERR_BAD_FORMAT       = 3,   /* format not one of FB_FMT_* */
    FB_ERR_BAD_DIMS         = 4,   /* zero or oversized width/height */
    FB_ERR_BAD_RECT         = 5,   /* write rectangle out of bounds */
    FB_ERR_BAD_BACKEND      = 6,   /* backend not one of FB_BACKEND_* */
    FB_ERR_BACKEND_BUSY     = 7,   /* HW_DIRECT: flip already in flight */
    FB_ERR_NO_SHMEM         = 8,   /* fb_shmem region not mapped */
    FB_ERR_BACKEND_UNAVAIL  = 9,   /* HW_DIRECT / REMOTE_API not yet implemented */
} fb_error_t;
