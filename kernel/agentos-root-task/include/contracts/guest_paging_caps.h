#ifndef AGENTOS_GUEST_PAGING_CAPS_H
#define AGENTOS_GUEST_PAGING_CAPS_H

/* ARM VMM-private paging allocation and ASID namespace. No capability here
 * names a peer VSpace or the system-wide ASID controller. Root moves the sole
 * paging-pool cap after completing guest mappings. Revoke only after stopping
 * execution and releasing all guest frame mappings. The ASID namespace is
 * management authority retained for subsequent guest VSpaces. */
#define AOS_GUEST_PAGING_POOL_CAP 463u
#define AOS_GUEST_ASID_POOL_CAP 464u
#define AOS_GUEST_PAGING_POOL_BITS 20u
#define AOS_GUEST_PAGING_TABLE_BASE 3600u
#define AOS_GUEST_PAGING_TABLE_COUNT 254u
/* Reserve up to 8 KiB for the VSpace and 254 intermediate 4 KiB tables.
 * These slots exclude RAM frame aliases and their stale-cap test slot. */
#include "guest_ram_caps.h"
_Static_assert(AOS_GUEST_PAGING_TABLE_BASE > AOS_GUEST_RAM_ALIAS_BASE + AOS_GUEST_RAM_MAX_FRAMES,
               "paging slots must not overlap RAM aliases");
_Static_assert(AOS_GUEST_PAGING_TABLE_BASE + AOS_GUEST_PAGING_TABLE_COUNT <= (1u << AOS_GUEST_RAM_CNODE_BITS),
               "paging slots must fit the VMM CNode");

#endif
