#ifndef AGENTOS_GUEST_GRAPHICS_CAPS_H
#define AGENTOS_GUEST_GRAPHICS_CAPS_H

/* ARM guest-owned graphics allocation: one 2 MiB queue page and twelve
 * 2 MiB surface-arena pages. Root moves the sole untyped caps to the owning
 * VMM. Service detach must precede revocation of all descendant mappings.
 * Observer queues/snapshots, physical display buffers and notifications are
 * excluded. The service never retains a guest pointer after detach ack.
 * Empty pools remain private reconstruction authority. */
#define AOS_GUEST_GRAPHICS_POOL_BASE 480u
#define AOS_GUEST_GRAPHICS_QUEUE_INDEX 0u
#define AOS_GUEST_GRAPHICS_ARENA_INDEX 1u
#define AOS_GUEST_GRAPHICS_ARENA_FRAMES 12u
#define AOS_GUEST_GRAPHICS_POOL_COUNT (1u + AOS_GUEST_GRAPHICS_ARENA_FRAMES)
#define AOS_GUEST_GRAPHICS_POOL_BITS 21u

#endif
