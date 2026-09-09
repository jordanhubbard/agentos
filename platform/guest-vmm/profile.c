#include <platform/guest_profile.h>

static bool add_fits(uint64_t base, uint64_t length, uint64_t limit)
{
    return length <= limit && base <= limit - length;
}

static bool hash_present(const uint8_t hash[32])
{
    uint8_t aggregate = 0u;
    for (size_t i = 0u; i < 32u; i++) aggregate |= hash[i];
    return aggregate != 0u;
}

static bool region_overlaps(uint64_t a, uint64_t a_length,
                            uint64_t b, uint64_t b_length)
{
    if (a_length == 0u || b_length == 0u) return false;
    if (!add_fits(a, a_length, UINT64_MAX) ||
        !add_fits(b, b_length, UINT64_MAX)) return true;
    return a < b + b_length && b < a + a_length;
}

static bool bytes_are_zero(const void *buffer, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)buffer;
    uint8_t aggregate = 0u;
    for (size_t i = 0u; i < length; i++) aggregate |= bytes[i];
    return aggregate == 0u;
}

bool aos_guest_profile_region_contains(const aos_guest_profile_manifest_t *profile,
                                       uint64_t address, uint64_t length)
{
    if (profile == NULL || address < profile->guest_gpa_base) return false;
    uint64_t offset = address - profile->guest_gpa_base;
    return add_fits(offset, length, profile->ram_size);
}

enum aos_guest_profile_error
aos_guest_profile_validate(const aos_guest_profile_manifest_t *profile)
{
    if (profile == NULL) return AOS_GUEST_PROFILE_ERR_NULL;
    if (profile->magic != AOS_GUEST_PROFILE_MAGIC ||
        profile->schema_version != AOS_GUEST_PROFILE_VERSION ||
        profile->manifest_size != AOS_GUEST_PROFILE_SIZE) {
        return AOS_GUEST_PROFILE_ERR_HEADER;
    }
    if (profile->architecture < AOS_GUEST_ARCH_AARCH64 ||
        profile->architecture > AOS_GUEST_ARCH_RISCV64 ||
        profile->boot_protocol < AOS_GUEST_BOOT_FDT_DIRECT ||
        profile->boot_protocol > AOS_GUEST_BOOT_PROCESS ||
        profile->kernel_format < AOS_GUEST_KERNEL_LINUX_IMAGE ||
        profile->kernel_format > AOS_GUEST_KERNEL_UEFI ||
        profile->control_type == 0u) {
        return AOS_GUEST_PROFILE_ERR_ENUM;
    }
    if ((profile->flags & ~(AOS_GUEST_PROFILE_AUTOSTART |
                            AOS_GUEST_PROFILE_HAS_INITRD |
                            AOS_GUEST_PROFILE_ENTRY_FROM_IMAGE |
                            AOS_GUEST_PROFILE_HASHED_ARTIFACTS)) != 0u) {
        return AOS_GUEST_PROFILE_ERR_FLAGS;
    }
    if (profile->profile_id_length == 0u ||
        profile->profile_id_length > AOS_GUEST_PROFILE_ID_MAX ||
        profile->profile_id[profile->profile_id_length] != '\0' ||
        profile->command_line_length > AOS_GUEST_PROFILE_CMDLINE_MAX ||
        profile->command_line[profile->command_line_length] != '\0') {
        return AOS_GUEST_PROFILE_ERR_TEXT;
    }
    if (!bytes_are_zero(profile->reserved, sizeof(profile->reserved)) ||
        !bytes_are_zero(&profile->profile_id[profile->profile_id_length + 1u],
                        sizeof(profile->profile_id) -
                            profile->profile_id_length - 1u) ||
        !bytes_are_zero(&profile->command_line[profile->command_line_length + 1u],
                        sizeof(profile->command_line) -
                            profile->command_line_length - 1u)) {
        return AOS_GUEST_PROFILE_ERR_TEXT;
    }
    if (profile->vcpu_count == 0u ||
        profile->vcpu_count > AOS_GUEST_PROFILE_VCPU_MAX ||
        profile->ram_size < UINT64_C(0x200000) ||
        (profile->ram_size & UINT64_C(0x1fffff)) != 0u ||
        (profile->guest_gpa_base & UINT64_C(0xfff)) != 0u ||
        (profile->vmm_hva_base & UINT64_C(0xfff)) != 0u ||
        !add_fits(profile->guest_gpa_base, profile->ram_size, UINT64_MAX) ||
        !add_fits(profile->vmm_hva_base, profile->ram_size, UINT64_MAX)) {
        return AOS_GUEST_PROFILE_ERR_MEMORY;
    }
    if (profile->kernel_max_bytes == 0u || profile->dtb_max_bytes == 0u ||
        !aos_guest_profile_region_contains(profile,
                                           profile->kernel_load_address,
                                           profile->kernel_max_bytes) ||
        !aos_guest_profile_region_contains(profile,
                                           profile->dtb_load_address,
                                           profile->dtb_max_bytes)) {
        return AOS_GUEST_PROFILE_ERR_ARTIFACT;
    }
    if (region_overlaps(profile->kernel_load_address,
                        profile->kernel_max_bytes,
                        profile->dtb_load_address,
                        profile->dtb_max_bytes)) {
        return AOS_GUEST_PROFILE_ERR_ARTIFACT;
    }
    if ((profile->flags & AOS_GUEST_PROFILE_HAS_INITRD) != 0u) {
        if (profile->initrd_max_bytes == 0u ||
            !aos_guest_profile_region_contains(profile,
                                               profile->initrd_load_address,
                                               profile->initrd_max_bytes)) {
            return AOS_GUEST_PROFILE_ERR_ARTIFACT;
        }
        if (region_overlaps(profile->kernel_load_address,
                            profile->kernel_max_bytes,
                            profile->initrd_load_address,
                            profile->initrd_max_bytes) ||
            region_overlaps(profile->dtb_load_address,
                            profile->dtb_max_bytes,
                            profile->initrd_load_address,
                            profile->initrd_max_bytes)) {
            return AOS_GUEST_PROFILE_ERR_ARTIFACT;
        }
    } else if (profile->initrd_load_address != 0u ||
               profile->initrd_max_bytes != 0u ||
               hash_present(profile->initrd_sha256)) {
        return AOS_GUEST_PROFILE_ERR_ARTIFACT;
    }
    if ((profile->flags & AOS_GUEST_PROFILE_ENTRY_FROM_IMAGE) == 0u &&
        !aos_guest_profile_region_contains(profile,
                                           profile->kernel_entry_address, 1u)) {
        return AOS_GUEST_PROFILE_ERR_ARTIFACT;
    }
    if ((profile->kernel_format == AOS_GUEST_KERNEL_LINUX_IMAGE) !=
        ((profile->flags & AOS_GUEST_PROFILE_ENTRY_FROM_IMAGE) != 0u)) {
        return AOS_GUEST_PROFILE_ERR_ARTIFACT;
    }
    if ((profile->flags & AOS_GUEST_PROFILE_HASHED_ARTIFACTS) != 0u &&
        (!hash_present(profile->profile_sha256) ||
         !hash_present(profile->kernel_sha256) ||
         !hash_present(profile->dtb_sha256) ||
         (((profile->flags & AOS_GUEST_PROFILE_HAS_INITRD) != 0u) &&
          !hash_present(profile->initrd_sha256)))) {
        return AOS_GUEST_PROFILE_ERR_ARTIFACT;
    }
    if ((profile->device_flags & ~AOS_GUEST_DEVICE_ALL) != 0u ||
        (profile->device_flags & AOS_GUEST_DEVICE_CONSOLE) == 0u ||
        (((profile->device_flags & AOS_GUEST_DEVICE_BLOCK) == 0u) &&
         profile->block_media != UINT16_MAX) ||
        (((profile->device_flags & AOS_GUEST_DEVICE_NET) == 0u) &&
         profile->network_client != UINT16_MAX)) {
        return AOS_GUEST_PROFILE_ERR_DEVICE;
    }
    return AOS_GUEST_PROFILE_OK;
}
