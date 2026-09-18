#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <platform/x86_profile.h>
#include <platform/x86_cpu.h>

int main(void)
{
    static const uint8_t abc_hash[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    aos_guest_profile_manifest_t p = {
        .magic=AOS_GUEST_PROFILE_MAGIC, .schema_version=AOS_GUEST_PROFILE_VERSION,
        .manifest_size=AOS_GUEST_PROFILE_SIZE, .architecture=AOS_GUEST_ARCH_X86_64,
        .boot_protocol=AOS_GUEST_BOOT_UEFI, .kernel_format=AOS_GUEST_KERNEL_UEFI,
        .flags=AOS_GUEST_PROFILE_AUTOSTART|AOS_GUEST_PROFILE_HAS_INITRD|AOS_GUEST_PROFILE_HASHED_ARTIFACTS,
        .vcpu_count=1, .control_type=1,
        .device_flags=AOS_GUEST_DEVICE_NET|AOS_GUEST_DEVICE_BLOCK|AOS_GUEST_DEVICE_CONSOLE,
        .ram_size=0x40000000, .vmm_hva_base=0x80000000,
        .kernel_load_address=0x200000, .initrd_load_address=0x8000000,
        .kernel_max_bytes=32, .initrd_max_bytes=32,
        .profile_id_length=4, .profile_id="test",
        .command_line_length=12, .command_line="console=hvc0",
    };
    p.profile_sha256[0]=1;
    memcpy(p.kernel_sha256,abc_hash,32);
    memcpy(p.initrd_sha256,abc_hash,32);
    aos_x86_boot_blobs_t b = {.kernel=(const uint8_t *)"abc", .kernel_size=3,
        .initrd=(const uint8_t *)"abc", .initrd_size=3,
        .cmdline=(const uint8_t *)"console=hvc0", .cmdline_size=13};
#define BIND() aos_x86_profile_bind(&p,sizeof(p),&b,0x40000000,0x80000000)
    assert(BIND());
    assert(!aos_x86_profile_bind(&p,sizeof(p)-1,&b,0x40000000,0x80000000));
    assert(!aos_x86_profile_bind(&p,sizeof(p),&b,0x20000000,0x80000000));
    p.vcpu_count=2; assert(!BIND()); p.vcpu_count=1;
    p.guest_id=1; assert(!BIND()); p.guest_id=0;
    p.network_client=1; assert(!BIND()); p.network_client=0;
    p.cpu_features.version=1; p.cpu_features.required=AOS_GUEST_CPU_FEATURE_RNG;
    assert(!BIND()); p.cpu_features.required=0;
    /* Exhaust the complete feature envelope, including contradictory masks.
     * The fixed model admits FP/SIMD requirements and forbids exposing the
     * other groups; it cannot honor a prohibition of its mandatory baseline. */
    for (unsigned required=0; required<=AOS_GUEST_CPU_FEATURE_ALL; required++) {
        for (unsigned prohibited=0; prohibited<=AOS_GUEST_CPU_FEATURE_ALL; prohibited++) {
            p.cpu_features.required=required;
            p.cpu_features.prohibited=prohibited;
            bool expected=(required & ~AOS_X86_CPU_PROFILE_FEATURES)==0 &&
                          (prohibited & AOS_X86_CPU_PROFILE_FEATURES)==0 &&
                          (required & prohibited)==0;
            assert(BIND()==expected);
        }
    }
    p.cpu_features.required=0; p.cpu_features.prohibited=0;
    p.kernel_sha256[0]^=1; assert(!BIND()); p.kernel_sha256[0]^=1;
    b.initrd=(const uint8_t *)"abd"; assert(!BIND()); b.initrd=(const uint8_t *)"abc";
    b.kernel_size=33; assert(!BIND()); b.kernel_size=3;
    b.cmdline=(const uint8_t *)"console=hvc1"; assert(!BIND());
    b.cmdline=(const uint8_t *)"console=hvc0";
    p.command_line[3]=0; assert(!BIND()); p.command_line[3]='s';
    b.cmdline_size=12; assert(!BIND()); b.cmdline_size=13;
    assert(BIND());
    puts("PASS: x86 profile binds provisioned resources, command line and golden SHA-256 artifacts");
    return 0;
}
