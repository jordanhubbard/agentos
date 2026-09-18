/*
 * Compiled guest runtime profile shared by host tooling and VMM PDs.
 *
 * Source profiles are TOML under guest-profiles/.  Target code consumes only
 * this fixed-size, bounded representation; acquisition and test recipes never
 * enter the target image.
 */
#ifndef AOS_PLATFORM_GUEST_PROFILE_H
#define AOS_PLATFORM_GUEST_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AOS_GUEST_PROFILE_MAGIC        UINT64_C(0x0046525047534f41)
#define AOS_GUEST_PROFILE_VERSION      2u
#define AOS_GUEST_PROFILE_SIZE         640u
#define AOS_GUEST_PROFILE_ID_MAX       63u
#define AOS_GUEST_PROFILE_CMDLINE_MAX  255u
#define AOS_GUEST_PROFILE_MEDIA_PATH_MAX 63u
#define AOS_GUEST_PROFILE_VCPU_MAX     8u
#define AOS_GUEST_PROFILE_RAM_MIN      UINT64_C(0x200000)
#define AOS_GUEST_PROFILE_RAM_MAX      UINT64_C(0x200000000)
#define AOS_GUEST_PROFILE_RAM_ALIGN    UINT64_C(0x200000)
#define AOS_GUEST_PROFILE_SELECTED_MAX 4u

/*
 * CPU feature requests are a bounded profile property, not host CPUID data.
 * Version zero with an all-zero payload is accepted solely for v2 manifests
 * produced before this field was assigned.  New compilers emit version one.
 *
 * Architecture-specific binding must enforce requests before guest execution.
 * The x86 firmware binder checks the fixed synthetic CPUID exposure (FP/SIMD),
 * rejecting unsupported requirements and prohibitions of mandatory features.
 * Envelope validation alone does not establish target support.
 */
#define AOS_GUEST_CPU_FEATURES_VERSION 1u

enum aos_guest_cpu_feature_flags {
    AOS_GUEST_CPU_FEATURE_FP          = 1u << 0,
    AOS_GUEST_CPU_FEATURE_SIMD        = 1u << 1,
    AOS_GUEST_CPU_FEATURE_CRYPTO      = 1u << 2,
    AOS_GUEST_CPU_FEATURE_RNG         = 1u << 3,
    AOS_GUEST_CPU_FEATURE_VECTOR      = 1u << 4,
    AOS_GUEST_CPU_FEATURE_NESTED_VIRT = 1u << 5,
};

#define AOS_GUEST_CPU_FEATURE_ALL ((1u << 6) - 1u)

typedef struct __attribute__((packed)) aos_guest_cpu_features {
    uint8_t version;
    uint8_t reserved;
    uint16_t required;
    uint16_t prohibited;
} aos_guest_cpu_features_t;

_Static_assert(sizeof(aos_guest_cpu_features_t) == 6u,
               "guest CPU feature wire size changed");

enum aos_guest_architecture {
    AOS_GUEST_ARCH_AARCH64 = 1u,
    AOS_GUEST_ARCH_X86_64  = 2u,
    AOS_GUEST_ARCH_RISCV64 = 3u,
};

enum aos_guest_boot_protocol {
    AOS_GUEST_BOOT_FDT_DIRECT = 1u,
    AOS_GUEST_BOOT_UEFI       = 2u,
    AOS_GUEST_BOOT_PROCESS    = 3u,
};

enum aos_guest_kernel_format {
    AOS_GUEST_KERNEL_LINUX_IMAGE = 1u,
    AOS_GUEST_KERNEL_RAW         = 2u,
    AOS_GUEST_KERNEL_ELF         = 3u,
    AOS_GUEST_KERNEL_UEFI        = 4u,
};

enum aos_guest_profile_flags {
    AOS_GUEST_PROFILE_AUTOSTART        = 1u << 0,
    AOS_GUEST_PROFILE_HAS_INITRD       = 1u << 1,
    AOS_GUEST_PROFILE_ENTRY_FROM_IMAGE = 1u << 2,
    AOS_GUEST_PROFILE_HASHED_ARTIFACTS = 1u << 3,
    AOS_GUEST_PROFILE_INITRD_FROM_MEDIA = 1u << 4,
};

enum aos_guest_device_flags {
    AOS_GUEST_DEVICE_NET     = 1u << 0,
    AOS_GUEST_DEVICE_BLOCK   = 1u << 1,
    AOS_GUEST_DEVICE_CONSOLE = 1u << 2,
    AOS_GUEST_DEVICE_GPU     = 1u << 3,
    AOS_GUEST_DEVICE_INPUT   = 1u << 4,
    AOS_GUEST_DEVICE_SOUND   = 1u << 5,
};

#define AOS_GUEST_DEVICE_ALL ((1u << 6) - 1u)

typedef struct __attribute__((packed)) aos_guest_profile_manifest {
    uint64_t magic;
    uint16_t schema_version;
    uint16_t manifest_size;
    uint8_t architecture;
    uint8_t boot_protocol;
    uint8_t kernel_format;
    uint8_t flags;
    uint32_t guest_id;
    uint32_t vcpu_count;
    uint32_t device_flags;
    uint16_t network_client;
    uint16_t block_media;
    uint64_t guest_gpa_base;
    uint64_t vmm_hva_base;
    uint64_t ram_size;
    uint64_t kernel_load_address;
    uint64_t kernel_entry_address;
    /* UEFI may omit DTB: address, maximum bytes and SHA-256 must all be zero. */
    uint64_t dtb_load_address;
    uint64_t initrd_load_address;
    uint64_t kernel_max_bytes;
    uint64_t dtb_max_bytes;
    uint64_t initrd_max_bytes;
    uint8_t profile_sha256[32];
    uint8_t kernel_sha256[32];
    uint8_t dtb_sha256[32];
    uint8_t initrd_sha256[32];
    uint16_t command_line_length;
    uint16_t profile_id_length;
    uint32_t control_type;
    uint16_t media_initrd_path_length;
    union {
        aos_guest_cpu_features_t cpu_features;
        /* Deprecated source-compatible name for the v2 reserved bytes. */
        uint8_t reserved[sizeof(aos_guest_cpu_features_t)];
    };
    char profile_id[64];
    char command_line[256];
    char media_initrd_path[64];
} aos_guest_profile_manifest_t;

_Static_assert(sizeof(aos_guest_profile_manifest_t) == AOS_GUEST_PROFILE_SIZE,
               "guest profile manifest wire size changed");

enum aos_guest_profile_error {
    AOS_GUEST_PROFILE_OK = 0,
    AOS_GUEST_PROFILE_ERR_NULL,
    AOS_GUEST_PROFILE_ERR_HEADER,
    AOS_GUEST_PROFILE_ERR_ENUM,
    AOS_GUEST_PROFILE_ERR_FLAGS,
    AOS_GUEST_PROFILE_ERR_TEXT,
    AOS_GUEST_PROFILE_ERR_MEMORY,
    AOS_GUEST_PROFILE_ERR_ARTIFACT,
    AOS_GUEST_PROFILE_ERR_DEVICE,
    AOS_GUEST_PROFILE_ERR_CPU,
    AOS_GUEST_PROFILE_ERR_RESOURCE,
};

enum aos_guest_profile_error
aos_guest_profile_validate(const aos_guest_profile_manifest_t *profile);

/*
 * Validate selected, concurrently mapped profiles.  Individual profiles are
 * validated first; guest GPA and VMM HVA ranges must then be disjoint, guest
 * IDs unique, and aggregate RAM bounded by the profile resource budget.
 */
enum aos_guest_profile_error
aos_guest_profile_validate_selected(
    const aos_guest_profile_manifest_t *const profiles[], size_t count);

bool aos_guest_profile_region_contains(const aos_guest_profile_manifest_t *profile,
                                       uint64_t address, uint64_t length);

#endif /* AOS_PLATFORM_GUEST_PROFILE_H */
