/* SPDX-License-Identifier: BSD-2-Clause */
#include <platform/gpu_framebuffer.h>
#include <string.h>

static bool call(aos_gpu_framebuffer_t *a, aos_fb_request_t q, aos_fb_response_t *p)
{
    q.version = AOS_FB_VERSION;
    return a->exchange(a->context, &q, p) && p->version == AOS_FB_VERSION &&
           p->status == AOS_FB_OK && p->id == q.id;
}
static bool create(void *ctx, uint32_t width, uint32_t height, uint64_t *handle)
{
    aos_gpu_framebuffer_t *a = ctx;
    aos_fb_response_t p;
    if (!call(a, (aos_fb_request_t){.operation=AOS_FB_CREATE,
            .width=width, .height=height}, &p) || !p.handle ||
            p.width != width || p.height != height) return false;
    *handle = p.handle;
    return true;
}
static bool destroy(void *ctx, uint64_t handle)
{
    aos_fb_response_t p;
    return call(ctx, (aos_fb_request_t){.operation=AOS_FB_DESTROY, .handle=handle}, &p);
}
static bool write_pixels(void *ctx, uint64_t handle, virtio_gpu_rect_t r, const void *pixels)
{
    aos_gpu_framebuffer_t *a = ctx;
    if (!pixels || !r.width || !r.height || r.width > AOS_FB_MAX_WIDTH ||
        r.height > AOS_FB_MAX_HEIGHT) return false;
    uint64_t bytes = (uint64_t)r.width * r.height * 4u;
    if (bytes > AOS_FB_DATA_BYTES) return false;
    memcpy(a->region->data, pixels, (size_t)bytes);
    aos_fb_response_t p;
    return call(a, (aos_fb_request_t){.operation=AOS_FB_WRITE, .handle=handle,
        .x=r.x, .y=r.y, .width=r.width, .height=r.height, .data_length=(uint32_t)bytes}, &p);
}
static bool flush(void *ctx, uint64_t handle, virtio_gpu_rect_t r)
{
    (void)r;
    aos_fb_response_t p;
    return call(ctx, (aos_fb_request_t){.operation=AOS_FB_FLIP, .handle=handle}, &p);
}
static bool scanout(void *ctx, uint64_t handle, virtio_gpu_rect_t r)
{
    aos_gpu_framebuffer_t *a = ctx;
    if (handle) {
        aos_fb_response_t p;
        if (!call(a, (aos_fb_request_t){.operation=AOS_FB_STATUS, .handle=handle}, &p) ||
            !r.width || !r.height || r.x >= p.width || r.y >= p.height ||
            r.width > p.width - r.x || r.height > p.height - r.y) return false;
    }
    a->scanout_handle = handle;
    a->scanout_rect = r;
    return true;
}
static bool validate(void *ctx, uint64_t gpa, uint32_t bytes)
{
    aos_gpu_framebuffer_t *a = ctx;
    return a->validate_gpa(a->context, gpa, bytes);
}
static bool cursor(void *ctx, bool update, uint64_t handle, uint32_t x, uint32_t y,
                   uint32_t hot_x, uint32_t hot_y)
{
    aos_gpu_framebuffer_t *a = ctx;
    if (handle) {
        aos_fb_response_t p;
        if (!call(a, (aos_fb_request_t){.operation=AOS_FB_STATUS, .handle=handle}, &p) ||
            p.width != 64 || p.height != 64 || hot_x >= 64 || hot_y >= 64) return false;
        /* UPDATE_CURSOR follows a fenced transfer, not RESOURCE_FLUSH.
         * Commit its shape now; MOVE_CURSOR must retain the existing shape. */
        if (update && !call(a, (aos_fb_request_t){.operation=AOS_FB_FLIP, .handle=handle}, &p)) return false;
    }
    a->cursor_handle = handle;
    a->cursor_x = x; a->cursor_y = y;
    a->cursor_hot_x = hot_x; a->cursor_hot_y = hot_y;
    return true;
}
static bool read_guest(void *ctx, uint64_t gpa, void *out, uint32_t bytes)
{
    aos_gpu_framebuffer_t *a = ctx;
    return a->read_gpa(a->context, gpa, out, bytes);
}
bool aos_gpu_framebuffer_init(aos_gpu_framebuffer_t *a, virtio_gpu_2d_t *g)
{
    if (!a || !a->region || !a->exchange || !a->validate_gpa || !a->read_gpa) return false;
    a->scanout_handle = 0;
    a->scanout_rect = (virtio_gpu_rect_t){0};
    a->cursor_handle = 0;
    a->cursor_x = a->cursor_y = a->cursor_hot_x = a->cursor_hot_y = 0;
    const virtio_gpu_2d_ops_t ops = {create, destroy, write_pixels, flush,
                                    scanout, cursor, validate, read_guest};
    return virtio_gpu_2d_init(g, &ops, a);
}
