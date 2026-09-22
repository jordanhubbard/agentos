/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <libvmm/virtio/virtio.h>
#include <libvmm/virtio/gpu_ring.h>

typedef struct {
    virtio_device_t device;
    virtio_queue_handler_t queues[2];
    virtio_gpu_ring_t rings[2];
    virtio_gpu_2d_t engine;
    uint32_t accepted_features[2];
    bool reset_failed;
    bool quiescing;
} virtio_gpu_device_t;

/* Initialize engine through virtio_gpu_2d_init (or the platform queue adapter)
 * first. The MMIO window must fault into this VMM, never map host GPU MMIO. */
bool virtio_mmio_gpu_init(virtio_gpu_device_t *, uintptr_t base, uintptr_t size, size_t virq);
/* After vCPUs stop and the current synchronous command returns, stop new
 * requests and release cursor, scanout and surfaces. Retry failed service
 * cleanup; guest reset cannot reopen admission. */
bool virtio_gpu_quiesce(virtio_gpu_device_t *);
