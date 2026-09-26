/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef AOS_VMM_VIRTIO_GPU_H
#define AOS_VMM_VIRTIO_GPU_H
#include <stdbool.h>
#include <platform/framebuffer.h>
/* After successful service COMMIT, local queue mapping, and retirement of
 * old fault/IRQ registrations. Failure retains the fresh attachment for
 * quiesce/detach cleanup; it must not be revoked before detach completes. */
bool aos_vmm_virtio_gpu_adopt(uint32_t client,uint32_t generation,aos_fb_region_t *fresh);
/* Stop execution first. Retry false until framebuffer cleanup succeeds. */
bool aos_vmm_virtio_gpu_quiesce(void);
/* After quiescence, await terminal framebuffer pointer retirement. */
bool aos_vmm_virtio_gpu_detach(void);
#if defined(ARCH_X86_64) || defined(__x86_64__)
#include <platform/x86_virtio.h>
#define AOS_VIRTIO_GPU_GUEST_IPA (AOS_X86_VIRTIO_BASE + 3u * AOS_X86_VIRTIO_STRIDE)
#define AOS_VIRTIO_GPU_VIRQ (AOS_X86_VIRTIO_GSI_BASE + 3u)
#else
#define AOS_VIRTIO_GPU_GUEST_IPA 0x0a040000UL
#define AOS_VIRTIO_GPU_VIRQ 54u
#endif
#define AOS_VIRTIO_GPU_MMIO_SIZE 0x1000UL
bool aos_vmm_virtio_gpu_init(void);
#endif
