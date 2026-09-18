/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <libvmm/virtio/virtio.h>

#define VIRTIO_INPUT_QUEUE_SIZE 128u
enum virtio_input_kind { VIRTIO_INPUT_KEYBOARD, VIRTIO_INPUT_POINTER };
/* The source is a platform virtualizer queue, never host device MMIO.
 * It returns one complete eight-byte little-endian event, or false when
 * empty. The backend requests an event only after validating output space.
 * Calls and notifications are serialized by the owning VMM. */
typedef bool (*virtio_input_receive_fn)(void *context, uint8_t event[8]);
typedef struct {
    virtio_device_t device;
    virtio_queue_handler_t queues[2];
    uint16_t consumed[2], produced[2];
    uint32_t accepted_features[2];
    struct virtq_desc descriptors[VIRTIO_INPUT_QUEUE_SIZE];
    virtio_input_receive_fn receive;
    void *context;
    enum virtio_input_kind kind;
    uint8_t select, subsel, event[8];
    bool held_event, failed, quiesced;
} virtio_input_device_t;

/* One device per keyboard or pointer, each at a faulting guest IPA/IRQ. */
bool virtio_mmio_input_init(virtio_input_device_t *, enum virtio_input_kind,
    virtio_input_receive_fn, void *context, uintptr_t base, uintptr_t size, size_t virq);
/* Call on a source notification as well as on eventq kicks. A full guest
 * queue leaves events in the source, preserving complete input sequences. */
bool virtio_input_drain(virtio_input_device_t *);
/* Stop vCPUs and finish the current callback first. Retire guest rings and
 * any held event; source queues remain owned by the input virtualizer.
 * Later notifications and guest reset cannot resume delivery. */
void virtio_input_quiesce(virtio_input_device_t *);
