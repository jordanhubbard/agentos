/* arch_barrier.h — portable memory barrier for seL4 Microkit PDs
 *
 * Abstracts architecture-specific full write memory barriers.
 * Include this instead of inline asm where barriers are needed.
 */
#pragma once

#if defined(__riscv)
  /* RISC-V: store-store fence */
  #define ARCH_WMB() __asm__ volatile ("fence w,w" ::: "memory")
  #define ARCH_MB()  __asm__ volatile ("fence"     ::: "memory")
#elif defined(__aarch64__)
  /* AArch64: data memory barrier inner-shareable */
  #define ARCH_WMB() __asm__ volatile ("dmb ish"   ::: "memory")
  #define ARCH_MB()  __asm__ volatile ("dsb ish"   ::: "memory")
#elif defined(__x86_64__) || defined(__i386__)
  #define ARCH_WMB() __asm__ volatile ("sfence"    ::: "memory")
  #define ARCH_MB()  __asm__ volatile ("mfence"    ::: "memory")
#else
  /* Compiler barrier only — sufficient for single-core / cache-coherent */
  #define ARCH_WMB() __asm__ volatile ("" ::: "memory")
  #define ARCH_MB()  __asm__ volatile ("" ::: "memory")
#endif
