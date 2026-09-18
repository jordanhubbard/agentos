#ifndef AOS_PLATFORM_X86_CONTROL_H
#define AOS_PLATFORM_X86_CONTROL_H

#include <sel4/sel4.h>
#include <platform/guest_vmm_runtime.h>

/* Run only between VM entries, after copying the complete VMEnter result
 * out of the IPC buffer. This call may overwrite every message register.
 * RUNNING permits re-entry; STOPPED requires another control step without
 * touching guest execution objects. ERROR is a malformed notification. */
/* The native path from VCPU revocation through the first reply uses general
 * registers only. SDK 2.3 can leave the retiring thread's FPU unavailable
 * until a scheduling transition: scalar IPC must not depend on an incidental
 * preemption or diagnostic Send to make compiler-generated SSE safe. */
enum aos_x86_control_result {
    AOS_X86_CONTROL_ERROR,
    AOS_X86_CONTROL_STOPPED,
    AOS_X86_CONTROL_RUNNING,
};
enum aos_x86_control_result aos_x86_control_step(
    const aos_guest_vmm_runtime_t *runtime,
    void (*wake)(seL4_Word badge, void *context), void *context);

/* Wait during device initialization without consuming a lifecycle request as
 * a notification. Early callers receive NOT_READY and can retry; no guest
 * state changes. A valid device notification is returned in wake_badge. */
bool aos_x86_control_wait_initializing(seL4_Word *wake_badge);

#endif
