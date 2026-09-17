/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <libvmm/virtio/gpu_2d.h>
#include <libvmm/virtio/virtq.h>

#define VIRTIO_GPU_QUEUE_SIZE 128u
typedef struct {
    bool (*validate)(void *, uint64_t, uint32_t);
    bool (*read)(void *, uint64_t, void *, uint32_t);
    bool (*write)(void *, uint64_t, const void *, uint32_t);
    void *context;
} virtio_gpu_ring_ops_t;

typedef struct {
    uint16_t last_index, used_index;
    bool failed;
    struct virtq_desc descriptors[VIRTIO_GPU_QUEUE_SIZE];
    uint8_t request[VIRTIO_GPU_2D_REQUEST_BYTES];
    uint8_t response[VIRTIO_GPU_2D_RESPONSE_BYTES];
} virtio_gpu_ring_t;

typedef struct { unsigned completed; bool valid; } virtio_gpu_ring_result_t;

/* ring pointers are already translated and mapped by the MMIO transport.
 * Descriptor addresses remain GPA and every byte copy uses the supplied
 * translator. State and descriptor snapshots are private to the VMM. Only
 * direct chains are accepted; indirect descriptors are not advertised.
 * Invalid rings stay stopped until the transport resets this state. */
virtio_gpu_ring_result_t virtio_gpu_control_run(virtio_gpu_ring_t *, struct virtq *,
    virtio_gpu_2d_t *, const virtio_gpu_ring_ops_t *, unsigned budget);
virtio_gpu_ring_result_t virtio_gpu_cursor_run(virtio_gpu_ring_t *, struct virtq *,
    virtio_gpu_2d_t *, const virtio_gpu_ring_ops_t *, unsigned budget);
