#ifndef AGENTOS_QUEUE_REBIND_CAPS_H
#define AGENTOS_QUEUE_REBIND_CAPS_H
/* Virtualizer-local management authority. Receive a private untyped and
 * allocate exactly one 2 MiB queue. The VMM retains the original pool and
 * can revoke every shared descendant. No device or peer VSpace grant. */
#define AOS_QUEUE_SERVICE_CNODE_BITS 10u
#define AOS_QUEUE_SERVICE_CNODE 512u
#define AOS_QUEUE_SERVICE_VSPACE 513u
#define AOS_QUEUE_SERVICE_RECEIVE 514u
#define AOS_QUEUE_SERVICE_FRAME_BASE 515u
#endif
