/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
/* Called after stopping guest execution; does not detach input_virt. */
void aos_vmm_virtio_input_quiesce(void);
#include <stdbool.h>
#include <stdint.h>
#include <platform/input.h>
/* After service REBIND, local mapping and fault/IRQ registry retirement.
 * A failed registration still owns the new attachment: detach it before
 * revoking the pool. Existing queued events are preserved. */
bool aos_vmm_virtio_input_adopt(uint32_t client, uint32_t generation,
                               aos_input_client_region_t *fresh);
/* Nonblocking terminal detach. False requires another call while the input
 * page remains mapped. Success stops all local adapter callbacks. */
bool aos_vmm_virtio_input_detach(void);
#if defined(ARCH_X86_64) || defined(__x86_64__)
#include <platform/x86_virtio.h>
#define AOS_VIRTIO_INPUT_KEYBOARD_IPA (AOS_X86_VIRTIO_BASE + 4u * AOS_X86_VIRTIO_STRIDE)
#define AOS_VIRTIO_INPUT_POINTER_IPA (AOS_X86_VIRTIO_BASE + 5u * AOS_X86_VIRTIO_STRIDE)
#define AOS_VIRTIO_INPUT_KEYBOARD_VIRQ (AOS_X86_VIRTIO_GSI_BASE + 4u)
#define AOS_VIRTIO_INPUT_POINTER_VIRQ (AOS_X86_VIRTIO_GSI_BASE + 5u)
#else
#define AOS_VIRTIO_INPUT_KEYBOARD_IPA 0x0a050000UL
#define AOS_VIRTIO_INPUT_POINTER_IPA 0x0a060000UL
#define AOS_VIRTIO_INPUT_KEYBOARD_VIRQ 55u
#define AOS_VIRTIO_INPUT_POINTER_VIRQ 56u
#endif
#define AOS_VIRTIO_INPUT_MMIO_SIZE 0x1000UL
bool aos_vmm_virtio_input_init(void);
void aos_vmm_virtio_input_drain(void);
