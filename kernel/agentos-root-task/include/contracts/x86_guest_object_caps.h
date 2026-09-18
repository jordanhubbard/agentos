/* Private x86 firmware guest execution and EPT allocation authority. */
#ifndef AGENTOS_X86_GUEST_OBJECT_CAPS_H
#define AGENTOS_X86_GUEST_OBJECT_CAPS_H

/* Architecture-exclusive with ARM's execution pool at this slot. The x86
 * VCPU is bound to the VMM TCB; that TCB must never belong to this pool.
 * Root moves the sole pool cap after setup. Revoke only outside VMEnter,
 * after guest I/O references have drained. RAM and its aliases are separate.
 * This grant alone does not implement suspend, destroy or reconstruction. */
#define AOS_X86_GUEST_OBJECT_POOL_CAP 461u
#define AOS_X86_GUEST_OBJECT_POOL_BITS 16u
#define AOS_X86_GUEST_OBJECT_COUNT 5u

/* Retained private ASID namespace, architecture-exclusive with ARM's ASID
 * grant. No global ASID controller is delegated. Intermediate EPT caps are
 * reconstructed only after the object pool has been completely revoked. */
#define AOS_X86_GUEST_ASID_POOL_CAP 464u
#define AOS_X86_GUEST_EPT_PDPT_CAP 498u
#define AOS_X86_GUEST_EPT_LOW_PD_CAP 499u
#define AOS_X86_GUEST_EPT_HIGH_PD_CAP 500u

#endif
