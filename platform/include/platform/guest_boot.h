/* Guest-neutral bounded direct-boot executor. */
#ifndef AOS_PLATFORM_GUEST_BOOT_H
#define AOS_PLATFORM_GUEST_BOOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <platform/guest_profile.h>

typedef struct aos_guest_boot_images {
    const void *kernel;
    size_t kernel_size;
    const void *dtb;
    size_t dtb_size;
    const void *initrd;
    size_t initrd_size;
} aos_guest_boot_images_t;

typedef uintptr_t (*aos_linux_image_loader_fn)(
    uintptr_t ram_start, uintptr_t kernel, size_t kernel_size,
    uintptr_t dtb_src, uintptr_t dtb_dest, size_t dtb_size,
    uintptr_t initrd_src, uintptr_t initrd_dest, size_t initrd_size);

typedef struct aos_guest_boot_plan {
    const aos_guest_profile_manifest_t *profile;
    uintptr_t kernel_hva;
    uintptr_t dtb_hva;
    uintptr_t initrd_hva;
    uintptr_t entry_gpa;
    uintptr_t dtb_gpa;
    uintptr_t initrd_gpa;
} aos_guest_boot_plan_t;

enum aos_guest_boot_error {
    AOS_GUEST_BOOT_OK = 0,
    AOS_GUEST_BOOT_ERR_ARGUMENT,
    AOS_GUEST_BOOT_ERR_PROFILE,
    AOS_GUEST_BOOT_ERR_SIZE,
    AOS_GUEST_BOOT_ERR_FORMAT,
    AOS_GUEST_BOOT_ERR_LOADER,
};

typedef struct aos_guest_device_ops {
    void (*net_init)(uint32_t client);
    void (*block_init)(uint32_t media);
    void (*console_init)(void);
} aos_guest_device_ops_t;

enum aos_guest_boot_error aos_guest_boot_prepare(
    aos_guest_boot_plan_t *plan,
    const aos_guest_profile_manifest_t *profile,
    uintptr_t guest_ram_hva,
    const aos_guest_boot_images_t *images,
    aos_linux_image_loader_fn linux_loader);

enum aos_guest_boot_error aos_guest_devices_init(
    const aos_guest_profile_manifest_t *profile,
    const aos_guest_device_ops_t *ops);

#endif /* AOS_PLATFORM_GUEST_BOOT_H */
