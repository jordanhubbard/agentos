/*
 * Framebuffer Device PD IPC Contract — Phase 4a
 *
 * Virtual framebuffer API for guest OSes and VibeOS.  Graphics is opt-in:
 * guests that never call MSG_FB_CREATE receive no framebuffer capability.
 *
 * Channel: CH_FB_PD (see agentos.h)
 * Opcodes: MSG_FB_* (see agentos.h)
 *
 * Lifecycle:
 *   MSG_FB_CREATE  → fb_handle
 *   MSG_FB_WRITE   → blit pixels via shared memory
 *   MSG_FB_FLIP    → commit frame (backend dispatch)
 *   MSG_FB_RESIZE  → change dimensions (backend-dependent)
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
 *   - MSG_FB_CREATE returns a handle; 0 is invalid (FB_HANDLE_INVALID).
 *   - MSG_FB_WRITE copies caller-supplied pixel data from the fb_shmem region.
 *   - MSG_FB_FLIP is the only operation that reaches the backend.
 *   - NULL backend: FLIP is accepted and frame_seq incremented; pixel data
 *     is silently discarded.  Suitable for headless guests and CI.
 *   - HW_DIRECT and REMOTE_API backends are defined in separate sub-beads.
 */

#pragma once
#include "../agentos.h"

/* ─── Handle sentinel ────────────────────────────────────────────────────── */

#define FB_HANDLE_INVALID  0u

/* ─── Pixel format constants ─────────────────────────────────────────────── */

#define FB_FMT_XRGB8888   0x00u
#define FB_FMT_ARGB8888   0x01u
#define FB_FMT_RGB565     0x02u

/* ─── Backend selection ──────────────────────────────────────────────────── */

typedef enum {
    FB_BACKEND_NULL       = 0,  /* headless: frames discarded after FLIP */
    FB_BACKEND_HW_DIRECT  = 1,  /* virtio-gpu / display controller (sub-bead ag-1es) */
    FB_BACKEND_REMOTE_API = 2,  /* host window-system relay (sub-bead ag-tz2) */
} fb_backend_t;

/* ─── MSG_FB_CREATE ──────────────────────────────────────────────────────── */

struct fb_create_req {
    uint32_t width;     /* pixels */
    uint32_t height;    /* pixels */
    uint32_t format;    /* FB_FMT_* */
    uint32_t backend;   /* fb_backend_t — caller may request; PD may downgrade */
};

struct fb_create_reply {
    uint32_t ok;
    uint32_t fb_handle;
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
    uint32_t x;         /* destination rectangle, pixels */
    uint32_t y;
    uint32_t w;
    uint32_t h;
    uint32_t src_stride; /* bytes per row of caller's source rectangle */
};

struct fb_write_reply {
    uint32_t ok;
};

/* ─── MSG_FB_FLIP ────────────────────────────────────────────────────────── */

struct fb_flip_req {
    uint32_t fb_handle;
};

struct fb_flip_reply {
    uint32_t ok;
    uint32_t frame_seq;  /* monotonically increasing frame counter */
};

/* ─── MSG_FB_RESIZE ──────────────────────────────────────────────────────── */

struct fb_resize_req {
    uint32_t fb_handle;
    uint32_t width;
    uint32_t height;
    uint32_t format;  /* FB_FMT_* — 0 = keep current */
};

struct fb_resize_reply {
    uint32_t ok;
    uint32_t actual_width;
    uint32_t actual_height;
    uint32_t stride;
    uint32_t buf_size;
    uint32_t shmem_offset;  /* may change if region was relocated */
};

/* ─── MSG_FB_DESTROY ─────────────────────────────────────────────────────── */

struct fb_destroy_req {
    uint32_t fb_handle;
};

struct fb_destroy_reply {
    uint32_t ok;
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
    uint32_t backend;       /* fb_backend_t */
    uint32_t frame_seq;     /* last completed frame */
    uint32_t flip_count;    /* total FLIPs since CREATE */
    uint32_t write_count;   /* total WRITEs since CREATE */
};

/* ─── MSG_FB_FRAME_READY (EventBus notification) ────────────────────────── */

/*
 * Published by framebuffer_pd after each successful FLIP.
 * kind field in agentos_event_t = EVT_FB_FRAME_READY.
 */
typedef struct __attribute__((packed)) {
    uint32_t fb_handle;
    uint32_t frame_seq;
    uint32_t width;
    uint32_t height;
    uint32_t format;   /* FB_FMT_* */
} fb_frame_ready_event_t;

#define EVT_FB_FRAME_READY  0x5001u

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum framebuffer_error {
    FB_OK                   = 0,
    FB_ERR_BAD_HANDLE       = 1,
    FB_ERR_NO_SLOTS         = 2,
    FB_ERR_BAD_FORMAT       = 3,
    FB_ERR_BAD_DIMS         = 4,  /* zero or oversized width/height */
    FB_ERR_BAD_RECT         = 5,  /* write rectangle out of bounds */
    FB_ERR_BAD_BACKEND      = 6,
    FB_ERR_BACKEND_BUSY     = 7,  /* HW_DIRECT: flip already in flight */
    FB_ERR_NO_SHMEM         = 8,  /* fb_shmem region not mapped */
};
