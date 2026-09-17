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
#endif
