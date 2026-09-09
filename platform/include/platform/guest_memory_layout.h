/*
 * Build-time guest RAM plan shared by the root allocator, VMMs, and manager.
 *
 * Each guest sees the conventional QEMU-virt GPA window at 0x40000000. The
 * VMM maps the same frames at a distinct HVA, so guest descriptor addresses
 * can never be dereferenced as host pointers.
 */
#ifndef AOS_PLATFORM_GUEST_MEMORY_LAYOUT_H
#define AOS_PLATFORM_GUEST_MEMORY_LAYOUT_H

#define AOS_PRIMARY_GUEST_GPA_BASE       0x40000000UL
#define AOS_SECONDARY_GUEST_GPA_BASE     0x40000000UL
#define AOS_PRIMARY_GUEST_RAM_BASE       0xc0000000UL
#define AOS_SECONDARY_GUEST_RAM_BASE     0x80000000UL

#if defined(AGENTOS_GUEST_DUAL)
#define AOS_PRIMARY_GUEST_RAM_SIZE       0x40000000UL
#define AOS_PRIMARY_GUEST_DTB_BASE       0x7f000000UL
#define AOS_PRIMARY_GUEST_INITRD_BASE    0x50000000UL
#define AOS_SECONDARY_GUEST_RAM_SIZE     0x10000000UL
#define AOS_SECONDARY_GUEST_DTB_BASE     0x4f000000UL
#else
#if defined(AGENTOS_GUEST_PRIMARY_LARGE)
#define AOS_PRIMARY_GUEST_RAM_SIZE       0x40000000UL
#define AOS_PRIMARY_GUEST_DTB_BASE       0x7f000000UL
#else
#define AOS_PRIMARY_GUEST_RAM_SIZE       0x20000000UL
#define AOS_PRIMARY_GUEST_DTB_BASE       0x5f000000UL
#endif
#define AOS_PRIMARY_GUEST_INITRD_BASE    0x50000000UL
#define AOS_SECONDARY_GUEST_RAM_SIZE     0x20000000UL
#define AOS_SECONDARY_GUEST_DTB_BASE     0x5f000000UL
#endif

#define AOS_PRIMARY_GUEST_RAM_MB \
    ((unsigned)(AOS_PRIMARY_GUEST_RAM_SIZE >> 20u))
#define AOS_SECONDARY_GUEST_RAM_MB \
    ((unsigned)(AOS_SECONDARY_GUEST_RAM_SIZE >> 20u))


#if defined(AGENTOS_GUEST_DUAL)
_Static_assert(AOS_SECONDARY_GUEST_RAM_BASE + AOS_SECONDARY_GUEST_RAM_SIZE <=
               AOS_PRIMARY_GUEST_RAM_BASE,
               "dual guest VMM HVA windows overlap");
#endif

#endif /* AOS_PLATFORM_GUEST_MEMORY_LAYOUT_H */
