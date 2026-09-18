/* Per-VMM RAM authority. No slot conveys another guest's memory. */
#ifndef AGENTOS_GUEST_RAM_CAPS_H
#define AGENTOS_GUEST_RAM_CAPS_H

#define AOS_GUEST_RAM_CNODE_BITS 12u
#define AOS_GUEST_RAM_SELF_CNODE 458u
#define AOS_GUEST_RAM_VMM_VSPACE 459u
#define AOS_GUEST_RAM_GUEST_VSPACE 460u
#define AOS_GUEST_RAM_POOL_BASE 512u
#define AOS_GUEST_RAM_MAX_FRAMES 1024u
#define AOS_GUEST_RAM_FRAME_BASE 1536u
#define AOS_GUEST_RAM_ALIAS_BASE 2560u
#define AOS_GUEST_RAM_FRAME_BITS 21u

/* Each pool is a non-device 2 MiB child untyped. Root moves its only cap
 * after creating the initial guest and VMM frame mappings. Revoking this
 * pool removes both mappings and all frame descendants, including root's
 * original caps. The pool itself survives for zeroed reallocation.
 * Reclamation requires stopped vCPUs and drained device work first.
 * VSpace caps retain the preallocated page tables needed for rebuilding.
 */
#endif
