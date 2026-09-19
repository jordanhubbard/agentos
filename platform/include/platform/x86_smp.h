#ifndef AOS_X86_SMP_H
#define AOS_X86_SMP_H
#include <stddef.h>
#include <platform/x86_apic.h>
#define AOS_X86_SMP_MAX_CPUS 32u
typedef enum {
    AOS_X86_CPU_RUNNING,
    AOS_X86_CPU_WAIT_SIPI,
    AOS_X86_CPU_START_PENDING
} aos_x86_cpu_start_state_t;
typedef struct {
    aos_x86_apic_t apic;
    aos_x86_cpu_start_state_t state;
    bool reset_pending;
    uint8_t startup_vector;
} aos_x86_smp_cpu_t;

/* Coordinator-owned contexts only, already admitted and uniquely identified.
 * Called between VM entries. This mutates virtual controller/startup state,
 * never allocates a CPU or invokes a native execution capability. Before any
 * later entry, the coordinator must apply reset_pending to the native VCPU
 * and START_PENDING to CS=(vector<<8), base=(vector<<12), IP=0. Only after
 * successful native setup may it clear reset_pending and mark RUNNING.
 * Sequence INIT-SIPI-SIPI starts a CPU once; INIT allows a later restart.
 * Failure leaves every context unchanged. Missing destinations are a no-op.
 * Supported delivery: fixed edge, INIT assert/deassert, SIPI. */
bool aos_x86_smp_icr(aos_x86_smp_cpu_t *, size_t count, unsigned sender,
                     uint32_t command, uint64_t ticks);
/* Bounded round robin starting after current, including current last. A CPU
 * awaiting reset/startup is not runnable. False leaves *next unchanged (no
 * runnable CPU or invalid arguments/state); caller continues lifecycle IPC. */
bool aos_x86_smp_next(const aos_x86_smp_cpu_t *, size_t count, unsigned current,
                      unsigned *next);
#endif
