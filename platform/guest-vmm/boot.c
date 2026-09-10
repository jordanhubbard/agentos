#include <platform/guest_boot.h>

static void copy_bytes(uintptr_t destination, const void *source, size_t size)
{
    volatile uint8_t *dst = (volatile uint8_t *)destination;
    const uint8_t *src = (const uint8_t *)source;
    for (size_t i = 0u; i < size; i++) dst[i] = src[i];
}

static bool image_fits(const aos_guest_profile_manifest_t *profile,
                       uint64_t address, size_t size, uint64_t maximum)
{
    return size <= maximum &&
           aos_guest_profile_region_contains(profile, address, size);
}

static uintptr_t guest_hva(const aos_guest_profile_manifest_t *profile,
                           uintptr_t ram_hva, uint64_t gpa)
{
    return ram_hva + (uintptr_t)(gpa - profile->guest_gpa_base);
}

enum aos_guest_boot_error aos_guest_boot_prepare(
    aos_guest_boot_plan_t *plan,
    const aos_guest_profile_manifest_t *profile,
    uintptr_t guest_ram_hva,
    const aos_guest_boot_images_t *images,
    aos_linux_image_loader_fn linux_loader)
{
    if (plan == NULL || profile == NULL || images == NULL ||
        images->kernel == NULL || images->dtb == NULL ||
        images->kernel_size == 0u || images->dtb_size == 0u) {
        return AOS_GUEST_BOOT_ERR_ARGUMENT;
    }
    if (aos_guest_profile_validate(profile) != AOS_GUEST_PROFILE_OK ||
        profile->boot_protocol != AOS_GUEST_BOOT_FDT_DIRECT) {
        return AOS_GUEST_BOOT_ERR_PROFILE;
    }
    bool has_initrd = (profile->flags & AOS_GUEST_PROFILE_HAS_INITRD) != 0u;
    if (!image_fits(profile, profile->kernel_load_address,
                    images->kernel_size, profile->kernel_max_bytes) ||
        !image_fits(profile, profile->dtb_load_address,
                    images->dtb_size, profile->dtb_max_bytes) ||
        (has_initrd && !image_fits(profile, profile->initrd_load_address,
                                   images->initrd_size,
                                   profile->initrd_max_bytes)) ||
        (!has_initrd && images->initrd_size != 0u)) {
        return AOS_GUEST_BOOT_ERR_SIZE;
    }

    uintptr_t kernel_hva = guest_hva(profile, guest_ram_hva,
                                     profile->kernel_load_address);
    uintptr_t dtb_hva = guest_hva(profile, guest_ram_hva,
                                  profile->dtb_load_address);
    uintptr_t initrd_hva = has_initrd
        ? guest_hva(profile, guest_ram_hva, profile->initrd_load_address)
        : 0u;
    uintptr_t entry_gpa;

    switch (profile->kernel_format) {
    case AOS_GUEST_KERNEL_LINUX_IMAGE: {
        if (linux_loader == NULL) return AOS_GUEST_BOOT_ERR_LOADER;
        uintptr_t loaded = linux_loader(
            kernel_hva, (uintptr_t)images->kernel, images->kernel_size,
            (uintptr_t)images->dtb, dtb_hva, images->dtb_size,
            (uintptr_t)images->initrd, initrd_hva, images->initrd_size);
        if (loaded < guest_ram_hva ||
            loaded - guest_ram_hva >= profile->ram_size) {
            return AOS_GUEST_BOOT_ERR_LOADER;
        }
        kernel_hva = loaded;
        entry_gpa = profile->guest_gpa_base + (loaded - guest_ram_hva);
        break;
    }
    case AOS_GUEST_KERNEL_RAW:
        copy_bytes(kernel_hva, images->kernel, images->kernel_size);
        copy_bytes(dtb_hva, images->dtb, images->dtb_size);
        if (images->initrd_size != 0u) {
            copy_bytes(initrd_hva, images->initrd, images->initrd_size);
        }
        entry_gpa = profile->kernel_entry_address;
        break;
    default:
        return AOS_GUEST_BOOT_ERR_FORMAT;
    }

    plan->profile = profile;
    plan->kernel_hva = kernel_hva;
    plan->dtb_hva = dtb_hva;
    plan->initrd_hva = initrd_hva;
    plan->entry_gpa = entry_gpa;
    plan->dtb_gpa = profile->dtb_load_address;
    plan->initrd_gpa = has_initrd ? profile->initrd_load_address : 0u;
    return AOS_GUEST_BOOT_OK;
}

enum aos_guest_boot_error aos_guest_devices_init(
    const aos_guest_profile_manifest_t *profile,
    const aos_guest_device_ops_t *ops)
{
    if (profile == NULL || ops == NULL) return AOS_GUEST_BOOT_ERR_ARGUMENT;
    if (aos_guest_profile_validate(profile) != AOS_GUEST_PROFILE_OK) {
        return AOS_GUEST_BOOT_ERR_PROFILE;
    }
    if ((profile->device_flags & AOS_GUEST_DEVICE_NET) != 0u) {
        if (ops->net_init == NULL) return AOS_GUEST_BOOT_ERR_ARGUMENT;
        ops->net_init(profile->network_client);
    }
    if ((profile->device_flags & AOS_GUEST_DEVICE_BLOCK) != 0u) {
        if (ops->block_init == NULL) return AOS_GUEST_BOOT_ERR_ARGUMENT;
        ops->block_init(profile->block_media);
    }
    if ((profile->device_flags & AOS_GUEST_DEVICE_CONSOLE) != 0u) {
        if (ops->console_init == NULL) return AOS_GUEST_BOOT_ERR_ARGUMENT;
        ops->console_init();
    }
    return AOS_GUEST_BOOT_OK;
}
