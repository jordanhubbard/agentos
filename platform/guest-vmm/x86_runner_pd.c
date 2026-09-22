#include <platform/x86_runner_native.h>
#include "sel4_ipc.h"

/* Private execution server. Root must supply a dedicated TCB/VSpace and bind
 * its admitted VCPU before publishing the badged endpoint to the coordinator.
 * No device, guest-memory mapping, nameserver registration or cap transfer. */
void pd_main(seL4_CPtr endpoint, seL4_CPtr nameserver)
{
    (void)nameserver;
    aos_x86_runner_t state;
    aos_x86_runner_init(&state);
    for (;;) {
        seL4_Word badge=0;
#ifdef CONFIG_KERNEL_MCS
        seL4_MessageInfo_t info=seL4_Recv(endpoint,&badge,AGENTOS_IPC_REPLY_CAP);
#else
        seL4_MessageInfo_t info=seL4_Recv(endpoint,&badge);
#endif
        aos_x86_runner_request_t request={0};
        aos_x86_runner_reply_t reply={0};
        bool accepted=false;
        /* Read MRs only after the complete envelope is accepted. Never accept
         * transferred capabilities or a bound notification as a command. */
        if (badge==AOS_X86_RUNNER_OWNER_BADGE &&
            seL4_MessageInfo_get_label(info)==AOS_X86_RUNNER_ENTER &&
            seL4_MessageInfo_get_length(info)==AOS_X86_RUNNER_REQUEST_WORDS &&
            !seL4_MessageInfo_get_extraCaps(info) &&
            !seL4_MessageInfo_get_capsUnwrapped(info)) {
            request.version=seL4_GetMR(0);
            request.sequence=seL4_GetMR(1);
            for (unsigned i=0; i<AOS_X86_RUNNER_ENTRY_WORDS; i++)
                request.entry[i]=seL4_GetMR(2+i);
            accepted=aos_x86_runner_step(&state,AOS_X86_RUNNER_ENTER,&request,
                AOS_X86_RUNNER_REQUEST_WORDS,aos_x86_runner_native_enter,NULL,&reply);
        }
        unsigned length=0;
        if (accepted) {
            seL4_SetMR(0,reply.version);
            seL4_SetMR(1,reply.sequence);
            seL4_SetMR(2,reply.result);
            seL4_SetMR(3,reply.badge);
            seL4_SetMR(4,reply.count);
            for (unsigned i=0; i<reply.count; i++)
                seL4_SetMR(AOS_X86_RUNNER_REPLY_HEADER_WORDS+i,reply.words[i]);
            length=AOS_X86_RUNNER_REPLY_HEADER_WORDS+reply.count;
        }
        info=seL4_MessageInfo_new(accepted ? AOS_X86_RUNNER_RETURN :
            AOS_X86_RUNNER_REJECT,0,0,length);
#ifdef CONFIG_KERNEL_MCS
        seL4_Send(AGENTOS_IPC_REPLY_CAP,info);
#else
        seL4_Reply(info);
#endif
    }
}
