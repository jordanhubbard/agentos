#include <platform/x86_profile.h>
#include <platform/x86_cpu.h>
#include "sha256_mini.h"

static bool equal(const uint8_t *a, const uint8_t *b, size_t length)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < length; i++) difference |= a[i] ^ b[i];
    return difference == 0;
}

bool aos_x86_profile_bind(const void *manifest, size_t manifest_bytes,
                          const aos_x86_boot_blobs_t *boot,
                          uint64_t ram_bytes, uint64_t ram_hva)
{
    if (!manifest || manifest_bytes != sizeof(aos_guest_profile_manifest_t) || !boot)
        return false;
    const aos_guest_profile_manifest_t *p = manifest;
    if (aos_guest_profile_validate(p) != AOS_GUEST_PROFILE_OK ||
        p->architecture != AOS_GUEST_ARCH_X86_64 ||
        p->boot_protocol != AOS_GUEST_BOOT_UEFI ||
        p->kernel_format != AOS_GUEST_KERNEL_UEFI ||
        p->flags != (AOS_GUEST_PROFILE_AUTOSTART | AOS_GUEST_PROFILE_HAS_INITRD |
                     AOS_GUEST_PROFILE_HASHED_ARTIFACTS) ||
        p->guest_id != 0 || p->control_type != 1 || p->vcpu_count != 1 ||
        p->device_flags != (AOS_GUEST_DEVICE_NET | AOS_GUEST_DEVICE_BLOCK | AOS_GUEST_DEVICE_CONSOLE) ||
        p->network_client != 0 || p->block_media != 0 ||
        (p->cpu_features.required & ~AOS_X86_CPU_PROFILE_FEATURES) != 0 ||
        (p->cpu_features.prohibited & AOS_X86_CPU_PROFILE_FEATURES) != 0 ||
        p->guest_gpa_base != 0 || p->ram_size != ram_bytes || p->vmm_hva_base != ram_hva ||
        p->kernel_entry_address != 0 || p->dtb_max_bytes != 0 ||
        !boot->kernel || !boot->kernel_size || boot->kernel_size > p->kernel_max_bytes ||
        boot->kernel_size > AOS_X86_BOOT_BLOB_LIMIT ||
        !boot->initrd || !boot->initrd_size || boot->initrd_size > p->initrd_max_bytes ||
        boot->initrd_size > AOS_X86_BOOT_BLOB_LIMIT ||
        !boot->cmdline || boot->cmdline_size != (uint32_t)p->command_line_length + 1u ||
        !equal(boot->cmdline, (const uint8_t *)p->command_line, boot->cmdline_size))
        return false;
    for (uint16_t i = 0; i < p->command_line_length; i++)
        if ((uint8_t)p->command_line[i] == 0 || (uint8_t)p->command_line[i] > 0x7f)
            return false;
    uint8_t digest[32];
    sha256_mini(boot->kernel, boot->kernel_size, digest);
    if (!equal(digest, p->kernel_sha256, sizeof(digest))) return false;
    sha256_mini(boot->initrd, boot->initrd_size, digest);
    return equal(digest, p->initrd_sha256, sizeof(digest));
}
