#include <assert.h>
#include <stdio.h>
#include <string.h>
/* Use the SDK's actual return-length definitions with instrumented syscalls. */
#define CONFIG_VTX 1
#define CONFIG_X86_64_VTX_64BIT_GUESTS 1
#include <platform/x86_vmenter.h>

static seL4_Word result, badge, registers[SEL4_VMENTER_RESULT_FAULT_LEN];
static unsigned reads, limit, enters;
seL4_Word seL4_VMEnter(seL4_Word *out)
{
    assert(out); *out=badge; enters++; return result;
}
seL4_Word seL4_GetMR(int index)
{
    assert(index>=0 && (unsigned)index<limit);
    assert((unsigned)index==reads++);
    return registers[index];
}
static aos_x86_vmenter_return_t capture(seL4_Word kind, unsigned count)
{
    result=kind; reads=0; limit=count;
    aos_x86_vmenter_return_t captured=aos_x86_vm_enter();
    assert(captured.result==kind && captured.badge==badge && reads==count);
    for (unsigned i=0;i<SEL4_VMENTER_RESULT_FAULT_LEN;i++)
        assert(captured.words[i]==(i<count ? registers[i] : 0));
    return captured;
}
int main(void)
{
    for (unsigned i=0;i<SEL4_VMENTER_RESULT_FAULT_LEN;i++) registers[i]=0xabc000+i;
    badge=0;
    aos_x86_vmenter_return_t fault=capture(SEL4_VMENTER_RESULT_FAULT,
                                          SEL4_VMENTER_RESULT_FAULT_LEN);
    badge=0x81;
    aos_x86_vmenter_return_t notification=capture(SEL4_VMENTER_RESULT_NOTIF,
                                                 SEL4_VMENTER_RESULT_NOTIF_LEN);
    assert(notification.words[SEL4_VMENTER_FAULT_REASON_MR]==0);
    /* Simulate a following IPC clobbering every message register. */
    memset(registers,0,sizeof(registers));
    assert(fault.words[SEL4_VMENTER_FAULT_REASON_MR]==0xabc000+SEL4_VMENTER_FAULT_REASON_MR);
    assert(notification.words[SEL4_VMENTER_CALL_EIP_MR]==0xabc000+SEL4_VMENTER_CALL_EIP_MR);
    capture(99,0);
    assert(enters==3);
    puts("PASS: fault snapshot, notification read boundary, badge and IPC independence");
}
