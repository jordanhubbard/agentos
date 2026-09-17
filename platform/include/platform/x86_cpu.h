#ifndef AOS_X86_CPU_H
#define AOS_X86_CPU_H
#include <stdbool.h>
#include <stdint.h>

typedef struct { uint32_t eax, ebx, ecx, edx; } aos_x86_cpuid_t;

/* Bootstrap CPU only: no APIC, VMX, XSAVE/AVX, MTRR, SEV or TDX promise.
 * The caller must validate the hardware baseline before running the guest. */
#define AOS_X86_BASIC_EDX ((1u<<0)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<6)| \
                         (1u<<8)|(1u<<13)|(1u<<15)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<26))
#define AOS_X86_EXT_EDX ((1u<<20)|(1u<<29))
bool aos_x86_cpu_supported(uint32_t basic_edx, uint32_t ext_edx, uint32_t widths);
aos_x86_cpuid_t aos_x86_cpu_id(uint32_t leaf, uint32_t subleaf);
/* Synthetic platform ID zero and no guest-loaded microcode. Host MSRs are
 * never read or written; update triggers and unknown registers are rejected. */
bool aos_x86_cpu_identity_msr(uint32_t msr, bool write, uint64_t *value);
#endif
