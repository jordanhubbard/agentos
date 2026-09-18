#ifndef AOS_PLATFORM_X86_RUNNER_NATIVE_H
#define AOS_PLATFORM_X86_RUNNER_NATIVE_H
#include <platform/x86_runner.h>
#include <platform/x86_vmenter.h>

/* Fail the build if a different SDK changes the private wire contract. */
_Static_assert(sizeof(seL4_Word)==sizeof(uint64_t), "runner requires x86-64 words");
_Static_assert(SEL4_VMENTER_RESULT_FAULT_LEN==AOS_X86_RUNNER_FAULT_WORDS,
               "runner fault snapshot ABI");
_Static_assert(SEL4_VMENTER_RESULT_NOTIF_LEN==AOS_X86_RUNNER_NOTIFICATION_WORDS,
               "runner notification snapshot ABI");
_Static_assert(SEL4_VMENTER_RESULT_FAULT==AOS_X86_RUNNER_FAULT &&
               SEL4_VMENTER_RESULT_NOTIF==AOS_X86_RUNNER_NOTIFICATION,
               "runner result ABI");

/* Executes only the VCPU bound to this calling native TCB. No IPC may run
 * between VMEnter and the snapshot copied by aos_x86_vm_start. */
static inline bool aos_x86_runner_native_enter(void *context,
    const uint64_t *words, aos_x86_runner_exit_t *out)
{
    (void)context;
    const aos_x86_vmenter_entry_t entry={words[0],words[1],words[2]};
    aos_x86_vmenter_return_t returned=aos_x86_vm_start(&entry);
    *out=(aos_x86_runner_exit_t){.result=returned.result,.badge=returned.badge};
    unsigned count=returned.result==SEL4_VMENTER_RESULT_FAULT ?
        SEL4_VMENTER_RESULT_FAULT_LEN : returned.result==SEL4_VMENTER_RESULT_NOTIF ?
        SEL4_VMENTER_RESULT_NOTIF_LEN : 0;
    for (unsigned i=0; i<count; i++) out->words[i]=returned.words[i];
    return count!=0;
}
#endif
