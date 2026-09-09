#include <platform/guest_boot.h>

#include <stdio.h>
#include <string.h>

#include "../../platform/guest-vmm/profile.c"
#include "../../platform/guest-vmm/boot.c"

static unsigned tests;
static unsigned failures;
static uint16_t net_client;
static uint16_t block_media;
static unsigned console_count;

#define CHECK(name, condition) do { \
    tests++; \
    if (condition) printf("ok %u - %s\n", tests, name); \
    else { printf("not ok %u - %s\n", tests, name); failures++; } \
} while (0)

static aos_guest_profile_manifest_t profile(uint8_t format)
{
    aos_guest_profile_manifest_t p;
    memset(&p, 0, sizeof(p));
    p.magic = AOS_GUEST_PROFILE_MAGIC;
    p.schema_version = AOS_GUEST_PROFILE_VERSION;
    p.manifest_size = sizeof(p);
    p.architecture = AOS_GUEST_ARCH_AARCH64;
    p.boot_protocol = AOS_GUEST_BOOT_FDT_DIRECT;
    p.kernel_format = format;
    p.flags = AOS_GUEST_PROFILE_HASHED_ARTIFACTS;
    if (format == AOS_GUEST_KERNEL_LINUX_IMAGE) {
        p.flags |= AOS_GUEST_PROFILE_ENTRY_FROM_IMAGE;
    }
    p.guest_id = 1u;
    p.vcpu_count = 1u;
    p.control_type = 1u;
    p.device_flags = AOS_GUEST_DEVICE_CONSOLE;
    p.network_client = UINT16_MAX;
    p.block_media = UINT16_MAX;
    p.guest_gpa_base = 0x40000000u;
    p.vmm_hva_base = 0x80000000u;
    p.ram_size = 0x02000000u;
    p.kernel_load_address = 0x40000000u;
    p.kernel_entry_address = 0x40001000u;
    p.dtb_load_address = 0x41f00000u;
    p.kernel_max_bytes = 0x01000000u;
    p.dtb_max_bytes = 0x00100000u;
    memset(p.profile_sha256, 1, 32);
    memset(p.kernel_sha256, 2, 32);
    memset(p.dtb_sha256, 3, 32);
    memcpy(p.profile_id, "test", 5);
    p.profile_id_length = 4u;
    return p;
}

static uintptr_t fake_linux_loader(
    uintptr_t ram, uintptr_t kernel, size_t kernel_size,
    uintptr_t dtb_src, uintptr_t dtb_dest, size_t dtb_size,
    uintptr_t initrd_src, uintptr_t initrd_dest, size_t initrd_size)
{
    (void)kernel; (void)kernel_size; (void)dtb_src; (void)dtb_dest;
    (void)dtb_size; (void)initrd_src; (void)initrd_dest; (void)initrd_size;
    return ram + 0x80000u;
}

static void net_init(uint32_t client) { net_client = (uint16_t)client; }
static void block_init(uint32_t media) { block_media = (uint16_t)media; }
static void console_init(void) { console_count++; }

int main(void)
{
    static uint8_t ram[0x02000000u];
    const uint8_t kernel[] = { 1u, 2u, 3u, 4u };
    const uint8_t dtb[] = { 0xd0u, 0x0du, 0xfeu, 0xedu };
    aos_guest_boot_images_t images = {
        .kernel = kernel, .kernel_size = sizeof(kernel),
        .dtb = dtb, .dtb_size = sizeof(dtb),
    };
    aos_guest_boot_plan_t plan;
    aos_guest_profile_manifest_t p = profile(AOS_GUEST_KERNEL_RAW);
    printf("TAP version 14\n# suite: guest_boot\n");
    CHECK("raw format uses declared entry",
          aos_guest_boot_prepare(&plan, &p, (uintptr_t)ram, &images, NULL) ==
              AOS_GUEST_BOOT_OK && plan.entry_gpa == p.kernel_entry_address);
    CHECK("raw executor copies kernel and DTB",
          memcmp(ram, kernel, sizeof(kernel)) == 0 &&
          memcmp(ram + 0x1f00000u, dtb, sizeof(dtb)) == 0);
    p = profile(AOS_GUEST_KERNEL_LINUX_IMAGE);
    CHECK("Linux Image entry comes from bounded loader",
          aos_guest_boot_prepare(&plan, &p, (uintptr_t)ram, &images,
                                 fake_linux_loader) == AOS_GUEST_BOOT_OK &&
          plan.entry_gpa == p.guest_gpa_base + 0x80000u);
    CHECK("Linux Image without loader is rejected",
          aos_guest_boot_prepare(&plan, &p, (uintptr_t)ram, &images, NULL) ==
              AOS_GUEST_BOOT_ERR_LOADER);
    images.kernel_size = p.kernel_max_bytes + 1u;
    CHECK("oversized embedded image is rejected before copy",
          aos_guest_boot_prepare(&plan, &p, (uintptr_t)ram, &images,
                                 fake_linux_loader) == AOS_GUEST_BOOT_ERR_SIZE);
    p = profile(AOS_GUEST_KERNEL_RAW);
    p.device_flags |= AOS_GUEST_DEVICE_NET | AOS_GUEST_DEVICE_BLOCK;
    p.network_client = 7u;
    p.block_media = 9u;
    aos_guest_device_ops_t ops = {
        .net_init = net_init,
        .block_init = block_init,
        .console_init = console_init,
    };
    CHECK("device endpoints are selected entirely by profile",
          aos_guest_devices_init(&p, &ops) == AOS_GUEST_BOOT_OK &&
          net_client == 7u && block_media == 9u && console_count == 1u);
    printf("1..%u\n", tests);
    return failures == 0u ? 0 : 1;
}
