/* SPDX-License-Identifier: BSD-2-Clause */
#include <libvmm/virtio/gpu_2d.h>
#include <string.h>

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t read64(const uint8_t *p)
{
    return read32(p) | (uint64_t)read32(p + 4) << 32;
}
static void write32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8u * i));
}
static virtio_gpu_rect_t rectangle(const uint8_t *p)
{
    return (virtio_gpu_rect_t){read32(p), read32(p+4), read32(p+8), read32(p+12)};
}
static virtio_gpu_resource_t *lookup(virtio_gpu_2d_t *g, uint32_t id)
{
    if (id)
        for (unsigned i = 0; i < VIRTIO_GPU_2D_RESOURCES; ++i)
            if (g->resources[i].id == id) return &g->resources[i];
    return NULL;
}
static bool valid_rect(const virtio_gpu_resource_t *s, virtio_gpu_rect_t r)
{
    return r.width && r.height && r.x < s->width && r.y < s->height &&
           r.width <= s->width - r.x && r.height <= s->height - r.y;
}
bool virtio_gpu_2d_init(virtio_gpu_2d_t *g, const virtio_gpu_2d_ops_t *ops, void *ctx)
{
    if (!g || !ops || !ops->create || !ops->destroy || !ops->write ||
        !ops->flush || !ops->scanout || !ops->cursor || !ops->validate_gpa || !ops->read_gpa) return false;
    memset(g, 0, sizeof(*g));
    g->ops = *ops;
    g->context = ctx;
    return true;
}
bool virtio_gpu_2d_reset(virtio_gpu_2d_t *g)
{
    if (!g) return false;
    if (g->cursor_resource) {
        if (!g->ops.cursor(g->context, true, 0, 0, 0, 0, 0)) return false;
        g->cursor_resource = 0;
    }
    if (g->scanout_resource) {
        if (!g->ops.scanout(g->context, 0, (virtio_gpu_rect_t){0})) return false;
        g->scanout_resource = 0;
    }
    bool ok = true;
    for (unsigned i = 0; i < VIRTIO_GPU_2D_RESOURCES; ++i) {
        virtio_gpu_resource_t *s = &g->resources[i];
        if (!s->id) continue;
        if (g->ops.destroy(g->context, s->handle)) memset(s, 0, sizeof(*s));
        else ok = false;
    }
    return ok;
}
static bool backing_read(virtio_gpu_2d_t *g, virtio_gpu_resource_t *s,
                         uint64_t offset, uint8_t *out, uint32_t length)
{
    for (unsigned i = 0; i < s->entries && length; ++i) {
        const virtio_gpu_backing_t *b = &s->backing[i];
        if (offset >= b->length) { offset -= b->length; continue; }
        uint32_t n = b->length - (uint32_t)offset;
        if (n > length) n = length;
        if (!g->ops.read_gpa(g->context, b->address + offset, out, n)) return false;
        out += n;
        length -= n;
        offset = 0;
    }
    return length == 0;
}
static uint32_t command(virtio_gpu_2d_t *g, const uint8_t *q, size_t n, uint8_t *p)
{
    uint32_t type = read32(q);
    if (read32(q + 16)) return GPU_ERR_INVALID_CONTEXT_ID;
    if (read32(q + 4) & ~1u) return GPU_ERR_INVALID_PARAMETER;
    if (type == GPU_GET_DISPLAY_INFO) {
        write32(p + 32, VIRTIO_GPU_2D_WIDTH);
        write32(p + 36, VIRTIO_GPU_2D_HEIGHT);
        write32(p + 40, 1);
        return GPU_OK_DISPLAY_INFO;
    }
    if (n < 32) return GPU_ERR_INVALID_PARAMETER;
    uint32_t id = read32(q + 24);
    if (type == GPU_RESOURCE_CREATE_2D) {
        if (n < 40 || !id || lookup(g, id)) return GPU_ERR_INVALID_PARAMETER;
        uint32_t format = read32(q + 28), width = read32(q + 32), height = read32(q + 36);
        if ((format != 1 && format != 2) || !width || !height ||
            width > VIRTIO_GPU_2D_WIDTH || height > VIRTIO_GPU_2D_HEIGHT)
            return GPU_ERR_INVALID_PARAMETER;
        for (unsigned i = 0; i < VIRTIO_GPU_2D_RESOURCES; ++i) {
            virtio_gpu_resource_t *s = &g->resources[i];
            if (s->id) continue;
            uint64_t handle = 0;
            if (!g->ops.create(g->context, width, height, &handle) || !handle)
                return GPU_ERR_OUT_OF_MEMORY;
            s->id = id;
            s->handle = handle;
            s->width = width;
            s->height = height;
            return GPU_OK_NODATA;
        }
        return GPU_ERR_OUT_OF_MEMORY;
    }
    virtio_gpu_rect_t r = {0};
    if (type == GPU_SET_SCANOUT || type == GPU_RESOURCE_FLUSH || type == GPU_TRANSFER_TO_HOST_2D) {
        if (n < (type == GPU_TRANSFER_TO_HOST_2D ? 56u : 48u)) return GPU_ERR_INVALID_PARAMETER;
        r = rectangle(q + 24);
        id = read32(q + (type == GPU_SET_SCANOUT ? 44 : type == GPU_RESOURCE_FLUSH ? 40 : 48));
    }
    if (type == GPU_SET_SCANOUT) {
        if (read32(q + 40)) return GPU_ERR_INVALID_SCANOUT_ID;
        if (!id) {
            if (!g->ops.scanout(g->context, 0, (virtio_gpu_rect_t){0})) return GPU_ERR_UNSPEC;
            g->scanout_resource = 0;
            return GPU_OK_NODATA;
        }
    }
    if (type < GPU_RESOURCE_UNREF || type > GPU_RESOURCE_DETACH_BACKING)
        return GPU_ERR_UNSPEC;
    virtio_gpu_resource_t *s = lookup(g, id);
    if (!s) return GPU_ERR_INVALID_RESOURCE_ID;
    switch (type) {
    case GPU_RESOURCE_UNREF:
        if (g->cursor_resource == id) {
            if (!g->ops.cursor(g->context, true, 0, 0, 0, 0, 0)) return GPU_ERR_UNSPEC;
            g->cursor_resource = 0;
        }
        if (g->scanout_resource == id) {
            if (!g->ops.scanout(g->context, 0, (virtio_gpu_rect_t){0})) return GPU_ERR_UNSPEC;
            g->scanout_resource = 0;
        }
        if (!g->ops.destroy(g->context, s->handle)) return GPU_ERR_UNSPEC;
        memset(s, 0, sizeof(*s));
        return GPU_OK_NODATA;
    case GPU_RESOURCE_ATTACH_BACKING: {
        uint32_t count = read32(q + 28);
        if (!count || count > VIRTIO_GPU_2D_BACKING_ENTRIES || s->entries ||
            n < 32u + (size_t)count * 16u) return GPU_ERR_INVALID_PARAMETER;
        uint64_t total = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t *e = q + 32u + i * 16u;
            uint64_t address = read64(e);
            uint32_t length = read32(e + 8);
            if (!length || address > UINT64_MAX - length ||
                !g->ops.validate_gpa(g->context, address, length)) return GPU_ERR_INVALID_PARAMETER;
            total += length; /* bounded by 1024 * UINT32_MAX */
        }
        if (total < (uint64_t)s->width * s->height * 4u) return GPU_ERR_INVALID_PARAMETER;
        for (uint32_t i = 0; i < count; ++i) {
            s->backing[i].address = read64(q + 32u + i * 16u);
            s->backing[i].length = read32(q + 40u + i * 16u);
        }
        s->entries = count;
        s->backing_bytes = total;
        return GPU_OK_NODATA;
    }
    case GPU_RESOURCE_DETACH_BACKING:
        s->entries = 0;
        s->backing_bytes = 0;
        return GPU_OK_NODATA;
    case GPU_SET_SCANOUT:
        if (!valid_rect(s, r)) return GPU_ERR_INVALID_PARAMETER;
        if (!g->ops.scanout(g->context, s->handle, r)) return GPU_ERR_UNSPEC;
        g->scanout_resource = id;
        return GPU_OK_NODATA;
    case GPU_RESOURCE_FLUSH:
        if (!valid_rect(s, r)) return GPU_ERR_INVALID_PARAMETER;
        return g->ops.flush(g->context, s->handle, r) ? GPU_OK_NODATA : GPU_ERR_UNSPEC;
    case GPU_TRANSFER_TO_HOST_2D: {
        if (!valid_rect(s, r) || !s->entries) return GPU_ERR_INVALID_PARAMETER;
        uint64_t offset = read64(q + 40), stride = (uint64_t)s->width * 4u;
        uint64_t extent = (r.height - 1u) * stride + r.width * 4u;
        if (offset > s->backing_bytes || extent > s->backing_bytes - offset)
            return GPU_ERR_INVALID_PARAMETER;
        const uint32_t row_bytes = r.width * 4u;
        const uint32_t batch_rows = sizeof(g->transfer) / row_bytes;
        for (uint32_t row = 0; row < r.height;) {
            uint32_t rows = r.height - row;
            if (rows > batch_rows) rows = batch_rows;
            /* Preserve backing stride while packing the destination payload.
             * Batches stay within the canonical queue's fixed byte bound. */
            for (uint32_t i = 0; i < rows; ++i)
                if (!backing_read(g, s, offset + (row + i) * stride,
                                  g->transfer + i * row_bytes, row_bytes))
                    return GPU_ERR_INVALID_PARAMETER;
            virtio_gpu_rect_t batch = {r.x, r.y + row, r.width, rows};
            if (!g->ops.write(g->context, s->handle, batch, g->transfer)) return GPU_ERR_UNSPEC;
            row += rows;
        }
        return GPU_OK_NODATA;
    }
    default: return GPU_ERR_UNSPEC;
    }
}

size_t virtio_gpu_2d_execute(virtio_gpu_2d_t *g, const void *request, size_t n,
                            void *response, size_t capacity)
{
    if (!g || !request || !response || n < 24 || capacity < 24 ||
        n > VIRTIO_GPU_2D_REQUEST_BYTES) return 0;
    const uint8_t *q = request;
    uint8_t *p = response;
    size_t required = read32(q) == GPU_GET_DISPLAY_INFO ? VIRTIO_GPU_2D_RESPONSE_BYTES : 24u;
    if (capacity < required) return 0;
    memset(p, 0, required);
    uint32_t result = command(g, q, n, p);
    write32(p, result);
    if (read32(q + 4) & 1u) {
        write32(p + 4, 1);
        memcpy(p + 8, q + 8, 8);
    }
    return result == GPU_OK_DISPLAY_INFO ? required : 24u;
}

bool virtio_gpu_2d_cursor(virtio_gpu_2d_t *g, const void *request, size_t n)
{
    if (!g || !request || n != 56) return false;
    const uint8_t *q = request;
    uint32_t type = read32(q), id = g->cursor_resource;
    if ((type != 0x300 && type != 0x301) || read32(q+24) || read32(q+16)) return false;
    uint32_t hot_x = g->cursor_hot_x, hot_y = g->cursor_hot_y;
    if (type == 0x300) {
        id = read32(q+40);
        hot_x = read32(q+44);
        hot_y = read32(q+48);
        if (id && (hot_x >= 64 || hot_y >= 64)) return false;
    }
    virtio_gpu_resource_t *s = lookup(g, id);
    if (id && (!s || s->width != 64 || s->height != 64)) return false;
    if (!g->ops.cursor(g->context, type == 0x300, s ? s->handle : 0,
                      read32(q+28), read32(q+32), hot_x, hot_y)) return false;
    g->cursor_resource = id;
    g->cursor_hot_x = hot_x;
    g->cursor_hot_y = hot_y;
    return true;
}
