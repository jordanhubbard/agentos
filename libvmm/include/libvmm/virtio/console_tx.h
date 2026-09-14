/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Private progress for a single available descriptor chain. Never retain a
 * pointer to mutable guest descriptor metadata across a backpressure wait. */
typedef struct {
    uint64_t address;
    uint32_t length;
    uint16_t flags, next;
} virtio_console_tx_descriptor_t;
typedef struct {
    virtio_console_tx_descriptor_t descriptor;
    uint32_t offset, traversed, transferred;
    uint16_t index;
    bool active, loaded, failed;
} virtio_console_tx_state_t;
typedef enum {
    VIRTIO_CONSOLE_TX_WAIT,
    VIRTIO_CONSOLE_TX_COMPLETE,
    VIRTIO_CONSOLE_TX_INVALID,
} virtio_console_tx_result_t;
typedef struct {
    bool (*descriptor)(void *, uint16_t, virtio_console_tx_descriptor_t *);
    /* Return accepted bytes, zero for full, UINT32_MAX for invalid GPA. */
    uint32_t (*copy)(void *, uint64_t, uint32_t, uint32_t);
    void *context;
} virtio_console_tx_ops_t;

/* Bounded by byte budget and queue_size descriptor snapshots per chain.
 * Only COMPLETE permits the caller to acknowledge the available head. */
static inline virtio_console_tx_result_t virtio_console_tx_step(
    virtio_console_tx_state_t *state, uint16_t head, uint32_t queue_size,
    uint32_t budget, const virtio_console_tx_ops_t *ops)
{
    state->transferred = 0;
    if (state->failed) return VIRTIO_CONSOLE_TX_INVALID;
    if (!state->active) {
        state->active = true;
        state->loaded = false;
        state->index = head;
        state->traversed = 0;
    }
    while (budget) {
        if (!state->loaded) {
            if (state->index >= queue_size || state->traversed >= queue_size ||
                !ops->descriptor(ops->context, state->index, &state->descriptor) ||
                (state->descriptor.flags & ~1u)) goto invalid;
            state->traversed++;
            state->offset = 0;
            state->loaded = true;
        }
        uint32_t remaining = state->descriptor.length - state->offset;
        if (remaining) {
            uint32_t request = remaining < budget ? remaining : budget;
            uint32_t copied = ops->copy(ops->context, state->descriptor.address,
                                         state->offset, request);
            if (copied > request) goto invalid;
            if (!copied) return VIRTIO_CONSOLE_TX_WAIT;
            state->offset += copied;
            state->transferred += copied;
            budget -= copied;
            if (state->offset != state->descriptor.length) continue;
        }
        if (!(state->descriptor.flags & 1u)) {
            state->active = false;
            state->loaded = false;
            return VIRTIO_CONSOLE_TX_COMPLETE;
        }
        state->index = state->descriptor.next;
        state->loaded = false;
    }
    return VIRTIO_CONSOLE_TX_WAIT;
invalid:
    state->failed = true;
    return VIRTIO_CONSOLE_TX_INVALID;
}
