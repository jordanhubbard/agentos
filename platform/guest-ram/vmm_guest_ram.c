#include <libvmm/virtio/gpa.h>
#include <platform/guest_ram.h>
#include <platform/guest_paging.h>
#include <contracts/guest_ram_caps.h>
#include <sel4/sel4.h>

void aos_vmm_guest_ram_bind(uint64_t gpa_base, uintptr_t hva_base, size_t size)
{
    aos_guest_ram_configure(gpa_base, hva_base, size);
    virtio_gpa_set_translate(aos_gpa_to_hva_configured);
}

static size_t ram_frame_count(size_t size)
{
    const size_t frame_size = (size_t)1u << AOS_GUEST_RAM_FRAME_BITS;
    if (size == 0u || (size & (frame_size - 1u)) != 0u ||
        size / frame_size > AOS_GUEST_RAM_MAX_FRAMES) return 0u;
    return size / frame_size;
}

bool aos_vmm_guest_ram_release(size_t size)
{
    size_t count = ram_frame_count(size);
    if (count == 0u) return false;
    /* Stop GPA translation before any mapping can disappear. */
    aos_guest_ram_configure(0u, 0u, 0u);
    for (size_t i = 0u; i < count; i++) {
        if (seL4_CNode_Revoke(AOS_GUEST_RAM_SELF_CNODE,
                AOS_GUEST_RAM_POOL_BASE + i, AOS_GUEST_RAM_CNODE_BITS)
                != seL4_NoError) return false;
    }
    return true;
}

bool aos_vmm_guest_ram_rebuild(uint64_t gpa, uintptr_t hva, size_t size)
{
    size_t count = ram_frame_count(size);
    const size_t frame_size = (size_t)1u << AOS_GUEST_RAM_FRAME_BITS;
    if (count == 0u || (gpa & (frame_size - 1u)) != 0u ||
        (hva & (frame_size - 1u)) != 0u ||
        gpa > UINT64_MAX - size || hva > UINTPTR_MAX - size) return false;

    for (size_t i = 0u; i < count; i++) {
        seL4_CPtr frame = AOS_GUEST_RAM_FRAME_BASE + i;
        seL4_CPtr alias = AOS_GUEST_RAM_ALIAS_BASE + i;
        if (seL4_Untyped_Retype(AOS_GUEST_RAM_POOL_BASE + i,
                seL4_ARM_LargePageObject, 0u, AOS_GUEST_RAM_SELF_CNODE,
                0u, 0u, frame, 1u) != seL4_NoError) return false;
        if (seL4_CNode_Copy(AOS_GUEST_RAM_SELF_CNODE, alias,
                AOS_GUEST_RAM_CNODE_BITS, AOS_GUEST_RAM_SELF_CNODE,
                frame, AOS_GUEST_RAM_CNODE_BITS, seL4_AllRights)
                != seL4_NoError) return false;
        if (!aos_vmm_guest_page_map(frame, gpa + i * frame_size)) return false;
        if (seL4_ARM_Page_Map(alias, AOS_GUEST_RAM_VMM_VSPACE,
                hva + i * frame_size, seL4_AllRights,
                seL4_ARM_Default_VMAttributes) != seL4_NoError) return false;
    }
    aos_vmm_guest_ram_bind(gpa, hva, size);
    return true;
}

#ifdef AGENTOS_GUEST_RAM_RECYCLE_TEST
bool aos_vmm_guest_ram_recycle_test(uint64_t gpa, uintptr_t hva, size_t size,
                                   bool (*restore_images)(void))
{
    if (ram_frame_count(size) == 0u || (hva & 7u) != 0u || !restore_images) return false;
    volatile uint64_t *ram = (volatile uint64_t *)hva;
    for (uint32_t pass = 0u; pass < 2u; pass++) {
        for (size_t i = 0u; i < size / sizeof(*ram); i++)
            ram[i] = UINT64_C(0xcafe012300000001) ^ (i << 1u) ^ pass;
        if (!aos_vmm_guest_ram_release(size)) return false;
        if (aos_gpa_to_hva_configured(gpa, 1u) != NULL) return false;
        /* Test deletion through a valid CNode invocation, so an absent
         * frame produces a lookup error rather than a capability fault. */
        if (pass != 0u && seL4_CNode_Copy(AOS_GUEST_RAM_SELF_CNODE,
                AOS_GUEST_RAM_ALIAS_BASE + AOS_GUEST_RAM_MAX_FRAMES,
                AOS_GUEST_RAM_CNODE_BITS, AOS_GUEST_RAM_SELF_CNODE,
                AOS_GUEST_RAM_FRAME_BASE, AOS_GUEST_RAM_CNODE_BITS,
                seL4_AllRights) != seL4_FailedLookup) return false;
        if (!aos_vmm_guest_ram_rebuild(gpa, hva, size)) return false;
        for (size_t i = 0u; i < size / sizeof(*ram); i++)
            if (ram[i] != 0u) return false;
        if (!restore_images()) return false;
    }
    return true;
}
#endif
