#ifndef AOS_X86_CPU_H
#define AOS_X86_CPU_H
#include <stdbool.h>
#include <stdint.h>
#include <platform/guest_profile.h>

/* Profile exposure for the synthetic CPU: FP means x87, SIMD means SSE/SSE2.
 * Crypto (AES/PCLMUL), RNG (RDRAND/RDSEED), vector (AVX family), and nested
 * virtualization are not advertised. This describes CPUID exposure, not an
 * instruction filter. The fixed baseline cannot disable FP or SIMD. */
#define AOS_X86_CPU_PROFILE_FEATURES \
    (AOS_GUEST_CPU_FEATURE_FP | AOS_GUEST_CPU_FEATURE_SIMD)

typedef struct { uint32_t eax, ebx, ecx, edx; } aos_x86_cpuid_t;

/* Bootstrap CPU only: local xAPIC, no VMX, XSAVE/AVX, MTRR, SEV or TDX promise.
 * The caller must validate the hardware baseline before running the guest. */
#define AOS_X86_BASIC_EDX ((1u<<0)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<6)| \
                         (1u<<8)|(1u<<9)|(1u<<13)|(1u<<15)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<26))
#define AOS_X86_EXT_EDX ((1u<<11)|(1u<<20)|(1u<<29))
bool aos_x86_cpu_supported(uint32_t basic_edx, uint32_t ext_edx, uint32_t widths);
/* Clock profile: the VMM must supply an admitted invariant TSC frequency.
 * The virtual crystal and undivided APIC clock both equal that frequency.
 * CPUID crystal Hz is 32-bit; reject clocks outside this explicit profile. */
bool aos_x86_cpu_clock_supported(uint64_t tsc_hz);
aos_x86_cpuid_t aos_x86_cpu_id(uint32_t leaf, uint32_t subleaf, uint64_t tsc_hz);
/* One package, one thread per core, contiguous APIC IDs [0,count), up to
 * 32 provisioned CPUs. Count one preserves the bootstrap CPUID profile.
 * Larger counts expose leaf 0xB SMT/core levels and leaf 1 identity/count.
 * Invalid topology leaves output unchanged; this does not admit resources. */
bool aos_x86_cpu_id_topology(uint32_t leaf, uint32_t subleaf, uint64_t tsc_hz,
                            unsigned count, unsigned apic_id,
                            aos_x86_cpuid_t *output);
/* Explicit clock discovery only: architectural ratio or QEMU/KVM's published
 * kHz leaf. Caller supplies zero leaves when their namespace is unavailable. */
uint64_t aos_x86_tsc_frequency(bool invariant, aos_x86_cpuid_t ratio,
                              aos_x86_cpuid_t hypervisor, aos_x86_cpuid_t timing);
/* Synthetic platform ID zero and no guest-loaded microcode. Host MSRs are
 * never read or written; update triggers and unknown registers are rejected. */
bool aos_x86_cpu_identity_msr(uint32_t msr, bool write, uint64_t *value);
/* Only the four syscall MSRs backed by seL4's per-VCPU context. No host MSR
 * access. Entry addresses must be canonical for the advertised 48-bit VA. */
bool aos_x86_cpu_syscall_msr(uint32_t msr, bool write, uint64_t value);
/* EFER write validation: SCE/LME/NXE writable, LMA derived from paging.
 * Failure leaves *next unchanged. LME cannot change while paging is on. */
bool aos_x86_cpu_efer(uint64_t current, uint64_t requested, bool paging,
                      uint64_t *next);
#endif
