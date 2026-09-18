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

#endif
