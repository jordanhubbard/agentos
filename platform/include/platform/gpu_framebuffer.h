/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef AOS_GPU_FRAMEBUFFER_H
#define AOS_GPU_FRAMEBUFFER_H
#include <platform/framebuffer.h>
#include <libvmm/virtio/gpu_2d.h>

/* One serialized queue client per VMM. exchange must submit, signal, wait
 * for and validate the matching response. It may not return before payload
 * ownership comes back to this client. This is a virtual scanout selection,
 * not a grant of physical display ownership or a hardware scanout driver. */
typedef struct {
    aos_fb_region_t *region;
    bool (*exchange)(void *context, const aos_fb_request_t *, aos_fb_response_t *);
    bool (*validate_gpa)(void *, uint64_t, uint32_t);
    bool (*read_gpa)(void *, uint64_t, void *, uint32_t);
    void *context;
    uint32_t next_id;
    uint64_t scanout_handle;
    virtio_gpu_rect_t scanout_rect;
    uint64_t cursor_handle;
    uint32_t cursor_x, cursor_y, cursor_hot_x, cursor_hot_y;
} aos_gpu_framebuffer_t;

bool aos_gpu_framebuffer_init(aos_gpu_framebuffer_t *, virtio_gpu_2d_t *);
#endif
