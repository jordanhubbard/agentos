#include <assert.h>
#include <stdio.h>
#include <string.h>
/* Use the SDK's actual return-length definitions with instrumented syscalls. */
#define CONFIG_VTX 1
#define CONFIG_X86_64_VTX_64BIT_GUESTS 1
#include <platform/x86_vmenter.h>
#include <platform/x86_runner_native.h>

static seL4_Word result, badge, registers[SEL4_VMENTER_RESULT_FAULT_LEN];
static unsigned reads, limit, enters, writes;
static const aos_x86_vmenter_entry_t *expected_entry;
void seL4_SetMR(int index, seL4_Word value)
{
    assert(index>=0 && (unsigned)index<SEL4_VMENTER_RESULT_NOTIF_LEN);
    assert((unsigned)index==writes++);
    registers[index]=value;
}
seL4_Word seL4_VMEnter(seL4_Word *out)
{
    if (expected_entry) {
        assert(writes == 3);
        assert(registers[SEL4_VMENTER_CALL_EIP_MR] == expected_entry->ip);
        assert(registers[SEL4_VMENTER_CALL_CONTROL_PPC_MR] == expected_entry->controls);
        assert(registers[2] == expected_entry->interruption_info);
        /* The kernel replaces entry inputs with exit state. */
        for (unsigned i = 0; i < limit; i++) registers[i] = 0xdef000 + i;
    }
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
    result=SEL4_VMENTER_RESULT_NOTIF; reads=0; limit=SEL4_VMENTER_RESULT_NOTIF_LEN;
    aos_x86_vmenter_return_t resumed=aos_x86_vm_resume_notification(&notification);
    assert(writes==SEL4_VMENTER_RESULT_NOTIF_LEN);
    assert(!memcmp(resumed.words,notification.words,sizeof(notification.words)));
    capture(99,0);
    assert(enters==4);
    const aos_x86_vmenter_entry_t entry = {
        .ip = 0xfff0, .controls = 0x80, .interruption_info = 0x80000031,
    };
    expected_entry = &entry;
    for (unsigned notification_exit = 0; notification_exit < 2; notification_exit++) {
        /* Startup CREATE/BOOT IPC must not become VM entry inputs. */
        memset(registers, 0xa5, sizeof(registers));
        writes = reads = 0;
        result = notification_exit ? SEL4_VMENTER_RESULT_NOTIF : SEL4_VMENTER_RESULT_FAULT;
        limit = notification_exit ? SEL4_VMENTER_RESULT_NOTIF_LEN : SEL4_VMENTER_RESULT_FAULT_LEN;
        badge = notification_exit ? 0x40 : 0;
        aos_x86_vmenter_return_t started = aos_x86_vm_start(&entry);
        assert(started.result == result && started.badge == badge && reads == limit);
        for (unsigned i = 0; i < SEL4_VMENTER_RESULT_FAULT_LEN; i++)
            assert(started.words[i] == (i < limit ? 0xdef000 + i : 0));
        assert(entry.ip == 0xfff0 && entry.controls == 0x80 &&
               entry.interruption_info == 0x80000031);
    }
    assert(enters == 6);
    aos_x86_runner_t runner;
    aos_x86_runner_init(&runner);
    aos_x86_runner_request_t request={.version=AOS_X86_RUNNER_VERSION,
        .sequence=1,.entry={entry.ip,entry.controls,entry.interruption_info}};
    for (unsigned notification_exit=0; notification_exit<2; notification_exit++) {
        writes=reads=0;
        result=notification_exit ? SEL4_VMENTER_RESULT_NOTIF : SEL4_VMENTER_RESULT_FAULT;
        limit=notification_exit ? SEL4_VMENTER_RESULT_NOTIF_LEN : SEL4_VMENTER_RESULT_FAULT_LEN;
        badge=notification_exit ? 0x40 : 0;
        memset(registers,0xa5,sizeof(registers));
        aos_x86_runner_reply_t reply;
        assert(aos_x86_runner_step(&runner,AOS_X86_RUNNER_ENTER,&request,
            AOS_X86_RUNNER_REQUEST_WORDS,aos_x86_runner_native_enter,NULL,&reply));
        assert(reads==limit && writes==3 && reply.count==limit);
        memset(registers,0xa5,sizeof(registers)); /* reply IPC clobbers MRs */
        assert(aos_x86_runner_reply_valid(&reply,
            AOS_X86_RUNNER_REPLY_HEADER_WORDS+limit,request.sequence));
        for (unsigned i=0; i<AOS_X86_RUNNER_FAULT_WORDS; i++)
            assert(reply.words[i]==(i<limit ? 0xdef000+i : 0));
        unsigned before=enters;
        assert(!aos_x86_runner_step(&runner,AOS_X86_RUNNER_ENTER,&request,
            AOS_X86_RUNNER_REQUEST_WORDS,aos_x86_runner_native_enter,NULL,&reply));
        assert(enters==before);
        request.sequence++;
    }
    assert(enters==8);
    puts("PASS: entry inputs survive startup IPC; fault/notification snapshots and re-entry");
}
