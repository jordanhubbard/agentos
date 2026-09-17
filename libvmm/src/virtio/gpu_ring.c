/* SPDX-License-Identifier: BSD-2-Clause */
#include <libvmm/virtio/gpu_ring.h>
#include <string.h>

static virtio_gpu_ring_result_t run(virtio_gpu_ring_t *s, struct virtq *r,
    virtio_gpu_2d_t *g, const virtio_gpu_ring_ops_t *ops, unsigned budget, bool cursor)
{
    virtio_gpu_ring_result_t result = {0};
    if (!s || !r || !g || !ops || !ops->validate || !ops->read || !ops->write) return result;
    if (s->failed) return result;
    unsigned size = r->num;
    if (!size || size > VIRTIO_GPU_QUEUE_SIZE || (size & (size-1)) ||
        !r->desc || !r->avail || !r->used) goto invalid;
    uint16_t available = __atomic_load_n(&r->avail->idx, __ATOMIC_ACQUIRE);
    unsigned pending = (uint16_t)(available - s->last_index);
    if (pending > size) goto invalid;
    if (budget > size) budget = size;
    while (pending && result.completed < budget) {
        uint16_t head = r->avail->ring[s->last_index % size], index = head;
        unsigned count = 0, first_write = size;
        size_t request_bytes = 0, response_bytes = 0;
        bool visited[VIRTIO_GPU_QUEUE_SIZE] = {0};
        for (;;) {
            if (index >= size || visited[index] || count >= size) goto invalid;
            visited[index] = true;
            struct virtq_desc d = r->desc[index];
            s->descriptors[count] = d;
            if (d.flags & ~(VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE)) goto invalid;
            if (d.addr > UINT64_MAX - d.len) goto invalid;
            if (d.flags & VIRTQ_DESC_F_WRITE) {
                if (cursor) goto invalid;
                if (first_write == size) first_write = count;
                /* Only the bounded reply prefix is accessed. Large guest
                 * buffer capacities must not enlarge our private copies. */
                size_t needed = sizeof(s->response) - response_bytes;
                uint32_t part = d.len < needed ? d.len : (uint32_t)needed;
                if (part && !ops->validate(ops->context, d.addr, part)) goto invalid;
                response_bytes += part;
            } else {
                if (first_write != size || d.len > sizeof(s->request) - request_bytes) goto invalid;
                if (d.len && !ops->read(ops->context, d.addr, s->request + request_bytes, d.len)) goto invalid;
                request_bytes += d.len;
            }
            ++count;
            if (!(d.flags & VIRTQ_DESC_F_NEXT)) break;
            index = d.next;
        }
        size_t length = 0;
        if (cursor) {
            if (request_bytes != 56) goto invalid;
            (void)virtio_gpu_2d_cursor(g, s->request, request_bytes);
        } else {
            length = virtio_gpu_2d_execute(g, s->request, request_bytes, s->response, response_bytes);
            if (!length) goto invalid;
        }
        size_t copied = 0;
        for (unsigned i = first_write; i < count && copied < length; ++i) {
            const struct virtq_desc *d = &s->descriptors[i];
            size_t part = d->len < length - copied ? d->len : length - copied;
            if (part && !ops->write(ops->context, d->addr, s->response + copied, (uint32_t)part)) goto invalid;
            copied += part;
        }
        if (copied != length) goto invalid;
        r->used->ring[s->used_index % size] = (struct virtq_used_elem){head, (uint32_t)length};
        ++s->used_index;
        __atomic_store_n(&r->used->idx, s->used_index, __ATOMIC_RELEASE);
        ++s->last_index;
        --pending;
        ++result.completed;
    }
    result.valid = true;
    return result;
invalid:
    s->failed = true;
    return result;
}

virtio_gpu_ring_result_t virtio_gpu_control_run(virtio_gpu_ring_t *s, struct virtq *r,
    virtio_gpu_2d_t *g, const virtio_gpu_ring_ops_t *ops, unsigned budget)
{
    return run(s,r,g,ops,budget,false);
}
virtio_gpu_ring_result_t virtio_gpu_cursor_run(virtio_gpu_ring_t *s, struct virtq *r,
    virtio_gpu_2d_t *g, const virtio_gpu_ring_ops_t *ops, unsigned budget)
{
    return run(s,r,g,ops,budget,true);
}
