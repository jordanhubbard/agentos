/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef AOS_VMM_VIRTIO_GPU_H
#define AOS_VMM_VIRTIO_GPU_H
#include <stdbool.h>
#define AOS_VIRTIO_GPU_GUEST_IPA 0x0a040000UL
#define AOS_VIRTIO_GPU_MMIO_SIZE 0x1000UL
#define AOS_VIRTIO_GPU_VIRQ 54u
bool aos_vmm_virtio_gpu_init(void);
#endif
