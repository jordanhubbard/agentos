/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
/* Called after stopping guest execution; does not detach input_virt. */
void aos_vmm_virtio_input_quiesce(void);
#include <stdbool.h>
#define AOS_VIRTIO_INPUT_KEYBOARD_IPA 0x0a050000UL
#define AOS_VIRTIO_INPUT_POINTER_IPA 0x0a060000UL
#define AOS_VIRTIO_INPUT_MMIO_SIZE 0x1000UL
#define AOS_VIRTIO_INPUT_KEYBOARD_VIRQ 55u
#define AOS_VIRTIO_INPUT_POINTER_VIRQ 56u
bool aos_vmm_virtio_input_init(void);
void aos_vmm_virtio_input_drain(void);
