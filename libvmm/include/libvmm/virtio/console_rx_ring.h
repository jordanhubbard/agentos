/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <libvmm/virtio/virtq.h>

#define VIRTIO_CONSOLE_RX_MAX_DESCRIPTORS 128u
typedef struct { uint16_t used; bool failed; } virtio_console_rx_state_t;
typedef struct { uint32_t bytes, completed; bool valid; } virtio_console_rx_result_t;
typedef struct {
    /* Mapping must cover the complete span for writing and remain stable. */
    void *(*map)(void *, uint64_t, uint32_t);
    /* Caller snapshots available input; take consumes exactly length bytes. */
    void (*take)(void *, void *, uint32_t);
    void *context;
} virtio_console_rx_ops_t;

/* Snapshot one bounded batch. Preflight each whole chain before consuming
 * input. A rejected head publishes no completion and consumes no input for
 * that head; previous completions in the batch remain valid. */
static inline virtio_console_rx_result_t virtio_console_rx_ring_run(
    struct virtq *ring, uint16_t *last, virtio_console_rx_state_t *state,
    uint32_t input, const virtio_console_rx_ops_t *ops)
{
    virtio_console_rx_result_t result={0};
    if (state->failed) return result;
    uint32_t size=ring->num;
    if (!size || size>VIRTIO_CONSOLE_RX_MAX_DESCRIPTORS ||
        !ring->desc || !ring->avail || !ring->used ||
        !ops || !ops->map || !ops->take ||
        __atomic_load_n(&ring->used->idx,__ATOMIC_RELAXED)!=state->used) goto invalid;
    uint16_t pending=(uint16_t)(__atomic_load_n(&ring->avail->idx,__ATOMIC_ACQUIRE)-*last);
    if (pending>size) goto invalid;
    while (pending && input) {
        struct { void *address; uint32_t length; } spans[VIRTIO_CONSOLE_RX_MAX_DESCRIPTORS];
        uint16_t head=__atomic_load_n(&ring->avail->ring[*last % size],__ATOMIC_RELAXED), index=head;
        uint32_t count=0;
        for (;;) {
            if (index>=size || count>=size) goto invalid;
            const struct virtq_desc *source=&ring->desc[index];
            struct virtq_desc d={
                .addr=__atomic_load_n(&source->addr,__ATOMIC_RELAXED),
                .len=__atomic_load_n(&source->len,__ATOMIC_RELAXED),
                .flags=__atomic_load_n(&source->flags,__ATOMIC_RELAXED),
                .next=__atomic_load_n(&source->next,__ATOMIC_RELAXED),
            };
            /* Only direct device-writable chains are negotiated. */
            if (!(d.flags & 2u) || (d.flags & ~3u)) goto invalid;
            void *address=d.len ? ops->map(ops->context,d.addr,d.len) : NULL;
            if (d.len && !address) goto invalid;
            spans[count].address=address; spans[count++].length=d.len;
            if (!(d.flags & 1u)) break;
            index=d.next;
        }
        uint32_t copied=0;
        for (uint32_t n=0;n<count && input;n++) {
            uint32_t bytes=spans[n].length<input ? spans[n].length : input;
            if (bytes) ops->take(ops->context,spans[n].address,bytes);
            input-=bytes; copied+=bytes;
        }
        ring->used->ring[state->used % size]=(struct virtq_used_elem){head,copied};
        state->used++;
        __atomic_store_n(&ring->used->idx,state->used,__ATOMIC_RELEASE);
        (*last)++; pending--; result.completed++; result.bytes+=copied;
    }
    result.valid=true;
    return result;
invalid:
    state->failed=true;
    return result;
}
