/*
 * Framebuffer Device PD IPC Contract
 *
 * The framebuffer_pd owns the display controller hardware via seL4 device
 * frame capabilities.  It exposes a simple linear framebuffer API for
 * guest OSes and the VibeOS virtual framebuffer concept.
 *
 * Channel: CH_FB_PD (see agentos.h)
 * Opcodes: MSG_FB_* (see agentos.h)
 *
 * Invariants:
 *   - MSG_FB_OPEN returns a handle; MSG_FB_MAP returns display geometry.
 *   - The framebuffer is double-buffered: two backing buffers (0 and 1);
 *     MSG_FB_FLIP performs atomic page flip between them.
 *   - The caller writes pixel data directly into the mapped framebuffer region
 *     (obtained via MSG_FB_MAP); seL4 shared memory mapping.
 *   - MSG_FB_CONFIGURE allows resolution and pixel format changes.
 *   - Only one client may hold a handle per display at a time.
 *   - This PD supports the "graphics as core virtual device metaphor" (Phase 4).
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define FB_PD_CH_CONTROLLER  CH_FB_PD

/* ─── Pixel format constants ─────────────────────────────────────────────── */

#define FB_FMT_XRGB8888   0x00u
#define FB_FMT_ARGB8888   0x01u
#define FB_FMT_RGB565     0x02u

/* ─── Request structs ────────────────────────────────────────────────────── */

struct fb_req_open {
    uint32_t disp_id;           /* 0 = primary display */
};

struct fb_req_close {
    uint32_t handle;
};

struct fb_req_map {
    uint32_t handle;
    /* reply contains geometry; caller uses seL4 shmem mapping to access pixels */
};

struct fb_req_unmap {
    uint32_t handle;
};

struct fb_req_flip {
    uint32_t handle;
    uint32_t buf_idx;           /* 0 or 1 — which buffer to make visible */
};

struct fb_req_status {
    uint32_t handle;
};

struct fb_req_configure {
    uint32_t handle;
    uint32_t width;             /* 0 = keep current */
    uint32_t height;
    uint32_t format;            /* FB_FMT_* */
    uint32_t refresh_hz;        /* 0 = keep current */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct fb_reply_open {
    uint32_t ok;
    uint32_t handle;
};

struct fb_reply_close {
    uint32_t ok;
};

struct fb_reply_map {
    uint32_t ok;
    uint32_t width;
    uint32_t height;
    uint32_t format;            /* FB_FMT_* */
    uint32_t stride;            /* bytes per row */
    uint64_t fb_phys;           /* physical base address of framebuffer region */
    uint32_t buf_size;          /* size of one backing buffer in bytes */
};

struct fb_reply_unmap {
    uint32_t ok;
};

struct fb_reply_flip {
    uint32_t ok;
    uint32_t vblank_count;      /* vertical blank count at time of flip */
};

struct fb_reply_status {
    uint32_t ok;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t refresh_hz;
    uint32_t mapped;            /* 1 = framebuffer currently mapped */
};

struct fb_reply_configure {
    uint32_t ok;
    uint32_t width;             /* actual width after configure */
    uint32_t height;
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum framebuffer_error {
    FB_OK                   = 0,
    FB_ERR_BAD_HANDLE       = 1,
    FB_ERR_BAD_DISP         = 2,
    FB_ERR_ALREADY_OPEN     = 3,  /* display already has a client */
    FB_ERR_NOT_MAPPED       = 4,
    FB_ERR_BAD_BUF_IDX      = 5,
    FB_ERR_BAD_FORMAT       = 6,
    FB_ERR_NO_HW            = 7,  /* no display hardware present */
};
