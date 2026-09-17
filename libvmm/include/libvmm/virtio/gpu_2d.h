/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* VirtIO 1.2, section 5.7.6. One unaccelerated display; no virgl/blob/EDID
 * features. Private bounds are never supplied by the guest. */
#define VIRTIO_GPU_2D_RESOURCES 4u
#define VIRTIO_GPU_2D_BACKING_ENTRIES 1024u
#define VIRTIO_GPU_2D_WIDTH 1024u
#define VIRTIO_GPU_2D_HEIGHT 768u
#define VIRTIO_GPU_2D_REQUEST_BYTES (32u + 16u * VIRTIO_GPU_2D_BACKING_ENTRIES)
#define VIRTIO_GPU_2D_RESPONSE_BYTES 408u

enum virtio_gpu_2d_command {
    GPU_GET_DISPLAY_INFO = 0x100, GPU_RESOURCE_CREATE_2D,
    GPU_RESOURCE_UNREF, GPU_SET_SCANOUT, GPU_RESOURCE_FLUSH,
    GPU_TRANSFER_TO_HOST_2D, GPU_RESOURCE_ATTACH_BACKING,
    GPU_RESOURCE_DETACH_BACKING
};
enum virtio_gpu_2d_response {
    GPU_OK_NODATA = 0x1100, GPU_OK_DISPLAY_INFO,
    GPU_ERR_UNSPEC = 0x1200, GPU_ERR_OUT_OF_MEMORY,
    GPU_ERR_INVALID_SCANOUT_ID, GPU_ERR_INVALID_RESOURCE_ID,
    GPU_ERR_INVALID_CONTEXT_ID, GPU_ERR_INVALID_PARAMETER
};

typedef struct {
    uint32_t x, y, width, height;
} virtio_gpu_rect_t;

/* All callbacks complete synchronously. The platform adapter owns the
 * framebuffer queue and persistent wakeups. Pixel pointers are temporary;
 * callbacks must copy them before returning. No physical GPU authority is
 * granted to this device backend. Handles are private backend identities. */
typedef struct {
    bool (*create)(void *, uint32_t width, uint32_t height, uint64_t *handle);
    bool (*destroy)(void *, uint64_t handle);
    bool (*write)(void *, uint64_t handle, virtio_gpu_rect_t rect, const void *pixels);
    bool (*flush)(void *, uint64_t handle, virtio_gpu_rect_t rect);
    bool (*scanout)(void *, uint64_t handle, virtio_gpu_rect_t rect);
    bool (*cursor)(void *, bool update, uint64_t handle, uint32_t x, uint32_t y,
                   uint32_t hot_x, uint32_t hot_y);
    bool (*validate_gpa)(void *, uint64_t address, uint32_t length);
    bool (*read_gpa)(void *, uint64_t address, void *output, uint32_t length);
} virtio_gpu_2d_ops_t;

typedef struct {
    uint64_t address;
    uint32_t length;
} virtio_gpu_backing_t;

typedef struct {
    uint32_t id, width, height, entries;
    uint64_t handle, backing_bytes;
    virtio_gpu_backing_t backing[VIRTIO_GPU_2D_BACKING_ENTRIES];
} virtio_gpu_resource_t;

typedef struct {
    virtio_gpu_2d_ops_t ops;
    void *context;
    uint32_t scanout_resource, cursor_resource;
    uint32_t cursor_hot_x, cursor_hot_y;
    virtio_gpu_resource_t resources[VIRTIO_GPU_2D_RESOURCES];
    uint8_t row[VIRTIO_GPU_2D_WIDTH * 4u];
} virtio_gpu_2d_t;

bool virtio_gpu_2d_init(virtio_gpu_2d_t *, const virtio_gpu_2d_ops_t *, void *context);
/* Request must be a private immutable snapshot, not a live guest pointer.
 * Output is also private and must not overlap the request. Returns response byte count, or zero when framing
 * is too short to respond. No operation runs without sufficient reply space.
 * Callers serialize commands and publish a response only after this returns. */
size_t virtio_gpu_2d_execute(virtio_gpu_2d_t *, const void *request, size_t length,
                            void *response, size_t capacity);
/* Release resources through the backend; retain any failed handles for retry. */
bool virtio_gpu_2d_reset(virtio_gpu_2d_t *);
/* Cursor queue commands have no response payload. False means the command
 * was ignored; it must not be reported as a successful visible update. */
bool virtio_gpu_2d_cursor(virtio_gpu_2d_t *, const void *request, size_t length);
