#ifndef AOS_X86_VMENTER_H
#define AOS_X86_VMENTER_H
#include <sel4/sel4.h>
#include <sel4/arch/vmenter.h>

/* Capture before any other IPC can overwrite the returned message registers.
 * A notification defines only the three re-entry words, not fault state. */
typedef struct {
    seL4_Word result, badge;
    seL4_Word words[SEL4_VMENTER_RESULT_FAULT_LEN];
} aos_x86_vmenter_return_t;

/* Explicit entry input, retained in native memory while startup IPC runs. */
typedef struct {
    seL4_Word ip, controls, interruption_info;
} aos_x86_vmenter_entry_t;

static inline aos_x86_vmenter_return_t aos_x86_vm_enter(void)
{
    aos_x86_vmenter_return_t returned = {0};
    returned.result = seL4_VMEnter(&returned.badge);
    unsigned count = returned.result == SEL4_VMENTER_RESULT_FAULT ?
        SEL4_VMENTER_RESULT_FAULT_LEN : returned.result == SEL4_VMENTER_RESULT_NOTIF ?
        SEL4_VMENTER_RESULT_NOTIF_LEN : 0;
    for (unsigned i = 0; i < count; i++) returned.words[i] = seL4_GetMR(i);
    return returned;
}

static inline aos_x86_vmenter_return_t aos_x86_vm_start(
    const aos_x86_vmenter_entry_t *entry)
{
    seL4_SetMR(SEL4_VMENTER_CALL_EIP_MR, entry->ip);
    seL4_SetMR(SEL4_VMENTER_CALL_CONTROL_PPC_MR, entry->controls);
    /* MR2 is interruption info in both SDKs; 2.3 renamed its enum. */
    _Static_assert(SEL4_VMENTER_RESULT_NOTIF_LEN == 3, "VMEnter input ABI");
    seL4_SetMR(2, entry->interruption_info);
    return aos_x86_vm_enter();
}

/* Call only for an accepted notification. Service IPC may have clobbered
 * MRs; restore all three seL4 re-entry words, including pending injection.
 * Guest GPRs remain in the VCPU and must not be rewritten from a fault copy. */
static inline aos_x86_vmenter_return_t aos_x86_vm_resume_notification(
    const aos_x86_vmenter_return_t *returned)
{
    for (unsigned i = 0; i < SEL4_VMENTER_RESULT_NOTIF_LEN; i++)
        seL4_SetMR(i, returned->words[i]);
    return aos_x86_vm_enter();
}
#endif
