/*
 * GPA → HVA hook for emulated virtio.
 *
 * QueueDesc/Avail/Used and virtq_desc.addr are guest physical addresses.
 * libvmm historically cast them to host pointers (identity map). The VMM
 * installs a bounds-checked translator via virtio_gpa_set_translate().
 * Until that is called, copies use identity (legacy / unconfigured).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct virtq;
struct virtio_queue_handler;

typedef void *(*virtio_gpa_translate_fn)(uint64_t gpa, size_t len);

void virtio_gpa_set_translate(virtio_gpa_translate_fn fn);
void *virtio_gpa_to_hva(uint64_t gpa, size_t len);

/* Copy helpers: fail closed (non-zero) on OOB or wrap. len == 0 succeeds. */
int virtio_copy_from_gpa(uint64_t gpa, size_t off, void *dst, size_t len);
int virtio_copy_to_gpa(uint64_t gpa, size_t off, const void *src, size_t len);

/*
 * virtq->desc/avail/used hold the GPA bit-pattern written through the queue
 * address registers until this maps them to host pointers. Call once when
 * QueueReady/QueueEnable goes 0→1.
 */
bool virtio_queue_map_guest_rings(struct virtq *virtq);

/* Restore all queue register and mapping state after a device reset. */
void virtio_queue_reset_guest_rings(struct virtio_queue_handler *handler);
