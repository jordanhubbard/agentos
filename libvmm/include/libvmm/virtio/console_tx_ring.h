/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <libvmm/virtio/console_tx.h>
#include <libvmm/virtio/virtq.h>

typedef uint32_t (*virtio_console_tx_copy_t)(void *, uint64_t, uint32_t, uint32_t);
typedef struct {
    struct virtq *ring;
    virtio_console_tx_copy_t copy;
    void *context;
} virtio_console_tx_ring_context_t;
typedef struct {
    uint32_t bytes, completed;
    bool valid;
} virtio_console_tx_ring_result_t;

static inline bool virtio_console_tx_ring_descriptor(void *ctx, uint16_t index,
    virtio_console_tx_descriptor_t *out)
{
    virtio_console_tx_ring_context_t *context = ctx;
    struct virtq_desc descriptor = context->ring->desc[index];
    *out = (virtio_console_tx_descriptor_t){descriptor.addr, descriptor.len,
                                          descriptor.flags, descriptor.next};
    return true;
}
static inline uint32_t virtio_console_tx_ring_copy(void *ctx, uint64_t address,
    uint32_t offset, uint32_t length)
{
    virtio_console_tx_ring_context_t *context = ctx;
    return context->copy(context->context, address, offset, length);
}

/* Actual available/used-ring handling, shared by target and host tests.
 * Snapshot availability once, retain the available head while blocked, and
 * publish a used entry only after the whole descriptor chain completes. */
static inline virtio_console_tx_ring_result_t virtio_console_tx_ring_run(
    struct virtq *ring, uint32_t maximum_size, uint16_t *last_index,
    uint16_t *head, virtio_console_tx_state_t *state, uint32_t budget,
    virtio_console_tx_copy_t copy, void *copy_context)
{
    virtio_console_tx_ring_result_t result = {0};
    if (state->failed) return result;
    uint32_t size = ring->num;
    if (!size || size > maximum_size || !ring->avail || !ring->used || !ring->desc)
        goto invalid;
    uint16_t available = __atomic_load_n(&ring->avail->idx, __ATOMIC_ACQUIRE);
    uint16_t pending = (uint16_t)(available - *last_index);
    if (pending > size || (state->active && !pending)) goto invalid;
    virtio_console_tx_ring_context_t context = {ring, copy, copy_context};
    const virtio_console_tx_ops_t ops = {
        virtio_console_tx_ring_descriptor, virtio_console_tx_ring_copy, &context
    };
    while (pending && result.bytes < budget) {
        if (!state->active) *head = ring->avail->ring[*last_index % size];
        virtio_console_tx_result_t step = virtio_console_tx_step(
            state, *head, size, budget - result.bytes, &ops);
        result.bytes += state->transferred;
        if (step == VIRTIO_CONSOLE_TX_INVALID) goto invalid;
        if (step == VIRTIO_CONSOLE_TX_WAIT) break;
        uint16_t used_index = ring->used->idx;
        ring->used->ring[used_index % size] = (struct virtq_used_elem){*head, 0};
        __atomic_store_n(&ring->used->idx, (uint16_t)(used_index + 1u), __ATOMIC_RELEASE);
        (*last_index)++;
        pending--;
        result.completed++;
    }
    result.valid = true;
    return result;
invalid:
    state->failed = true;
    return result;
}
