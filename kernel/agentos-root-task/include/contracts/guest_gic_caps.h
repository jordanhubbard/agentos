#ifndef AGENTOS_GUEST_GIC_CAPS_H
#define AGENTOS_GUEST_GIC_CAPS_H
#include "guest_scheduling_caps.h"

/* ARM manager-only mapping authority for the virtual GIC CPU interface.
 * Root moves each guest's existing mapping cap to the manager. The VMM
 * exports its own VSpace in its isolated exchange, never a device cap.
 * CREATE must leave execution stopped; manager prepares this fixed IPA
 * before BOOT. Reconstruction supplies a new ASID-assigned VSpace with
 * intermediate page tables at this IPA before replying to CREATE. */
#define AOS_GUEST_GIC_VSPACE_EXCHANGE_SLOT 3u
#define AOS_GUEST_GIC_VSPACE_SCRATCH 609u
#define AOS_GUEST_GIC_FRAME_BASE 610u
#define AOS_GUEST_GIC_IPA 0x08010000UL
_Static_assert(AOS_GUEST_GIC_VSPACE_EXCHANGE_SLOT < (1u << AOS_GUEST_SCHED_EXCHANGE_BITS),
               "guest VSpace must fit the private exchange");
_Static_assert(AOS_GUEST_GIC_VSPACE_SCRATCH >= AOS_GUEST_SCHED_SCRATCH_BASE + AOS_GUEST_SCHED_OBJECTS,
               "guest mapping scratch must not overlap scheduling copies");
_Static_assert(AOS_GUEST_GIC_FRAME_BASE > AOS_GUEST_GIC_VSPACE_SCRATCH &&
               AOS_GUEST_GIC_FRAME_BASE + AOS_GUEST_SCHED_CLIENTS <= (1u << AOS_GUEST_SCHED_MANAGER_BITS),
               "guest mapping caps must fit distinct manager slots");
#endif
