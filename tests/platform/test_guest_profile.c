#include <platform/guest_profile.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../platform/guest-vmm/profile.c"

static unsigned tests;
static unsigned failures;

#define CHECK(name, condition) do { \
    tests++; \
    if (condition) printf("ok %u - %s\n", tests, name); \
    else { printf("not ok %u - %s\n", tests, name); failures++; } \
} while (0)

static aos_guest_profile_manifest_t valid_profile(void)
{
    aos_guest_profile_manifest_t p;
    memset(&p, 0, sizeof(p));
    p.magic = AOS_GUEST_PROFILE_MAGIC;
    p.schema_version = AOS_GUEST_PROFILE_VERSION;
    p.manifest_size = sizeof(p);
    p.architecture = AOS_GUEST_ARCH_AARCH64;
    p.boot_protocol = AOS_GUEST_BOOT_FDT_DIRECT;
    p.kernel_format = AOS_GUEST_KERNEL_LINUX_IMAGE;
    p.flags = AOS_GUEST_PROFILE_HAS_INITRD |
              AOS_GUEST_PROFILE_ENTRY_FROM_IMAGE |
              AOS_GUEST_PROFILE_HASHED_ARTIFACTS;
    p.guest_id = 7u;
    p.vcpu_count = 1u;
    p.control_type = 1u;
    p.device_flags = AOS_GUEST_DEVICE_NET | AOS_GUEST_DEVICE_BLOCK |
                     AOS_GUEST_DEVICE_CONSOLE;
    p.network_client = 0u;
    p.block_media = 0u;
    p.guest_gpa_base = UINT64_C(0x40000000);
    p.vmm_hva_base = UINT64_C(0xc0000000);
    p.ram_size = UINT64_C(0x20000000);
    p.kernel_load_address = UINT64_C(0x40000000);
    p.dtb_load_address = UINT64_C(0x5f000000);
    p.initrd_load_address = UINT64_C(0x50000000);
    p.kernel_max_bytes = UINT64_C(0x08000000);
    p.dtb_max_bytes = UINT64_C(0x00100000);
    p.initrd_max_bytes = UINT64_C(0x08000000);
    memset(p.profile_sha256, 1, sizeof(p.profile_sha256));
    memset(p.kernel_sha256, 2, sizeof(p.kernel_sha256));
    memset(p.dtb_sha256, 3, sizeof(p.dtb_sha256));
    memset(p.initrd_sha256, 4, sizeof(p.initrd_sha256));
    memcpy(p.profile_id, "proof-aarch64", 14u);
    p.profile_id_length = 13u;
    memcpy(p.command_line, "console=hvc0", 13u);
    p.command_line_length = 12u;
    return p;
}

static void move_profile(aos_guest_profile_manifest_t *p,
                         uint64_t gpa_base, uint64_t hva_base)
{
    uint64_t offset = gpa_base - p->guest_gpa_base;
    p->guest_gpa_base = gpa_base;
    p->vmm_hva_base = hva_base;
    p->kernel_load_address += offset;
    p->kernel_entry_address += offset;
    p->dtb_load_address += offset;
    p->initrd_load_address += offset;
}

static void check_compiled_manifest(const char *path)
{
    aos_guest_profile_manifest_t p;
    FILE *file = fopen(path, "rb");
    bool read_ok = file != NULL && fread(&p, 1u, sizeof(p), file) == sizeof(p);
    int trailing = read_ok ? fgetc(file) : EOF;
    if (file != NULL) fclose(file);
    CHECK(path, read_ok && trailing == EOF &&
                aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_OK);
}

int main(int argc, char **argv)
{
    aos_guest_profile_manifest_t p = valid_profile();
    printf("TAP version 14\n# suite: guest_profile\n");
    CHECK("wire layout is fixed", sizeof(p) == AOS_GUEST_PROFILE_SIZE);
    CHECK("valid bounded profile accepted",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_OK);
    p.architecture = AOS_GUEST_ARCH_X86_64;
    p.boot_protocol = AOS_GUEST_BOOT_UEFI;
    p.kernel_format = AOS_GUEST_KERNEL_UEFI;
    p.flags &= ~AOS_GUEST_PROFILE_ENTRY_FROM_IMAGE;
    p.kernel_entry_address = p.kernel_load_address;
    CHECK("UEFI profile may retain a DTB",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_OK);
    p.dtb_max_bytes = 0;
    p.dtb_load_address = 0;
    memset(p.dtb_sha256, 0, sizeof(p.dtb_sha256));
    CHECK("UEFI profile may omit DTB with canonical zero fields",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_OK);
    p.dtb_load_address = p.kernel_load_address;
    CHECK("absent DTB rejects stale load address",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_ARTIFACT);
    p.dtb_load_address = 0;
    p.dtb_sha256[0] = 1;
    CHECK("absent DTB rejects stale hash",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_ARTIFACT);
    p.dtb_sha256[0] = 0;
    p.boot_protocol = AOS_GUEST_BOOT_FDT_DIRECT;
    CHECK("FDT boot still requires DTB",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_ARTIFACT);
    p = valid_profile();
    CHECK("kernel window belongs to guest RAM",
          aos_guest_profile_region_contains(&p, p.kernel_load_address,
                                            p.kernel_max_bytes));
    CHECK("wrapped address rejected",
          !aos_guest_profile_region_contains(&p, UINT64_MAX - 4u, 16u));
    p.magic ^= 1u;
    CHECK("wrong manifest magic rejected",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_HEADER);
    p = valid_profile();
    p.ram_size++;
    CHECK("unaligned RAM size rejected",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_MEMORY);
    p = valid_profile();
    p.vcpu_count = 0u;
    CHECK("zero vCPU count rejected",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_MEMORY);
    p.vcpu_count = AOS_GUEST_PROFILE_VCPU_MAX + 1u;
    CHECK("vCPU count above bound rejected",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_MEMORY);
    p = valid_profile();
    p.dtb_load_address = p.guest_gpa_base + p.ram_size - 16u;
    CHECK("out-of-range DTB rejected",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_ARTIFACT);
    p = valid_profile();
    memset(p.kernel_sha256, 0, sizeof(p.kernel_sha256));
    CHECK("missing required artifact identity rejected",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_ARTIFACT);
    p = valid_profile();
    p.device_flags &= ~AOS_GUEST_DEVICE_CONSOLE;
    CHECK("profile without bounded console rejected",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_DEVICE);
    p = valid_profile();
    p.profile_id[p.profile_id_length] = 'x';
    CHECK("unterminated profile id rejected",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_TEXT);
    p = valid_profile();
    p.initrd_load_address = p.kernel_load_address + 0x1000u;
    CHECK("overlapping maximum artifact windows rejected",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_ARTIFACT);
    p = valid_profile();
    p.cpu_features.version = AOS_GUEST_CPU_FEATURES_VERSION + 1u;
    CHECK("unknown CPU feature version rejected",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_CPU);
    p = valid_profile();
    p.cpu_features.version = AOS_GUEST_CPU_FEATURES_VERSION;
    p.cpu_features.required =
        AOS_GUEST_CPU_FEATURE_FP | AOS_GUEST_CPU_FEATURE_SIMD;
    CHECK("versioned bounded CPU features accepted",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_OK);
    p.cpu_features.prohibited = AOS_GUEST_CPU_FEATURE_FP;
    CHECK("conflicting CPU feature policy rejected",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_CPU);
    p = valid_profile();
    p.vcpu_count = AOS_GUEST_PROFILE_VCPU_MAX;
    p.ram_size = AOS_GUEST_PROFILE_RAM_MAX;
    p.dtb_load_address = p.guest_gpa_base + p.ram_size - p.dtb_max_bytes;
    CHECK("maximum bounded vCPU and multi-GiB RAM accepted",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_OK);
    p.ram_size += AOS_GUEST_PROFILE_RAM_ALIGN;
    CHECK("RAM above resource bound rejected",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_MEMORY);
    p = valid_profile();
    p.profile_id[p.profile_id_length + 1u] = 'x';
    CHECK("hidden text after terminator rejected",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_TEXT);
    p = valid_profile();
    p.kernel_format = AOS_GUEST_KERNEL_RAW;
    CHECK("format and entry strategy mismatch rejected",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_ARTIFACT);
    p = valid_profile();
    p.control_type = 0u;
    CHECK("zero lifecycle control type rejected",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_ENUM);
    p = valid_profile();
    memcpy(p.media_initrd_path, "boot/initrd", 12u);
    p.media_initrd_path_length = 11u;
    p.flags |= AOS_GUEST_PROFILE_INITRD_FROM_MEDIA;
    CHECK("bounded media initrd path accepted",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_OK);
    p.device_flags &= ~AOS_GUEST_DEVICE_BLOCK;
    p.block_media = UINT16_MAX;
    CHECK("media initrd requires profile block device",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_ARTIFACT);
    p = valid_profile();
    memcpy(p.media_initrd_path, "../initrd", 10u);
    p.media_initrd_path_length = 9u;
    p.flags |= AOS_GUEST_PROFILE_INITRD_FROM_MEDIA;
    CHECK("parent traversal in media path rejected",
          aos_guest_profile_validate(&p) == AOS_GUEST_PROFILE_ERR_ARTIFACT);
    {
        aos_guest_profile_manifest_t first = valid_profile();
        aos_guest_profile_manifest_t second = valid_profile();
        const aos_guest_profile_manifest_t *selected[] = { &first, &second };
        second.guest_id = first.guest_id + 1u;
        move_profile(&second, first.guest_gpa_base + first.ram_size,
                     first.vmm_hva_base + first.ram_size);
        CHECK("disjoint selected profile placements accepted",
              aos_guest_profile_validate_selected(selected, 2u) ==
                  AOS_GUEST_PROFILE_OK);
        move_profile(&second, first.guest_gpa_base + first.ram_size - 0x1000u,
                     second.vmm_hva_base);
        CHECK("selected profile GPA overlap rejected",
              aos_guest_profile_validate_selected(selected, 2u) ==
                  AOS_GUEST_PROFILE_ERR_RESOURCE);
        move_profile(&second, first.guest_gpa_base + first.ram_size,
                     first.vmm_hva_base + first.ram_size - 0x1000u);
        CHECK("selected profile HVA overlap rejected",
              aos_guest_profile_validate_selected(selected, 2u) ==
                  AOS_GUEST_PROFILE_ERR_RESOURCE);
    }
    {
        aos_guest_profile_manifest_t desktop = valid_profile();
        aos_guest_profile_manifest_t small = valid_profile();
        const aos_guest_profile_manifest_t *selected[] = { &desktop, &small };
        desktop.ram_size = AOS_GUEST_PROFILE_RAM_MAX;
        desktop.dtb_load_address =
            desktop.guest_gpa_base + desktop.ram_size - desktop.dtb_max_bytes;
        small.guest_id = desktop.guest_id + 1u;
        small.ram_size = AOS_GUEST_PROFILE_RAM_ALIGN;
        small.kernel_max_bytes = UINT64_C(0x80000);
        small.initrd_max_bytes = UINT64_C(0x80000);
        small.dtb_max_bytes = UINT64_C(0x1000);
        move_profile(&small, desktop.guest_gpa_base + desktop.ram_size,
                     desktop.vmm_hva_base + desktop.ram_size);
        small.initrd_load_address = small.guest_gpa_base + UINT64_C(0x100000);
        small.dtb_load_address = small.guest_gpa_base + UINT64_C(0x1ff000);
        CHECK("8 GiB aligned desktop profile accepted",
              aos_guest_profile_validate(&desktop) == AOS_GUEST_PROFILE_OK);
        CHECK("8 GiB plus 2 MiB selected RAM exceeds aggregate budget",
              aos_guest_profile_validate_selected(selected, 2u) ==
                  AOS_GUEST_PROFILE_ERR_RESOURCE);
    }
    for (int i = 1; i < argc; i++) check_compiled_manifest(argv[i]);
    printf("1..%u\n", tests);
    return failures == 0u ? 0 : 1;
}
