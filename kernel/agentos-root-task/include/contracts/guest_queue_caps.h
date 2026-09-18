#ifndef AGENTOS_GUEST_QUEUE_CAPS_H
#define AGENTOS_GUEST_QUEUE_CAPS_H

/* ARM VMM-private non-device untyped pools, one existing 2 MiB client frame
 * each. Root moves the only pool cap after mapping the frame into its owning
 * VMM and service. Revocation removes all descendant frame capabilities and
 * mappings, including root's originals. No driver, native/operator, frontend
 * or peer frame belongs to these pools. Detach every service before revoke.
 * Empty pools survive as bounded authority for a future reconstruction path.
 * Graphics queues/surfaces and notification objects are separate resources. */
#define AOS_GUEST_QUEUE_POOL_BASE 465u
#define AOS_GUEST_QUEUE_NET 0u
#define AOS_GUEST_QUEUE_BLOCK 1u
#define AOS_GUEST_QUEUE_SERIAL 2u
#define AOS_GUEST_QUEUE_INPUT 3u
#define AOS_GUEST_QUEUE_POOL_COUNT 4u
#define AOS_GUEST_QUEUE_POOL_BITS 21u
/* Qualification-only retype destination; empty in normal images. */
#define AOS_GUEST_QUEUE_TEST_FRAME 469u
#define AOS_GUEST_QUEUE_TEST_COPY 470u
#endif
