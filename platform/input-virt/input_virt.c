/* SPDX-License-Identifier: BSD-2-Clause */
#include <stdbool.h>
#include <platform/input.h>
#include <platform/input_rebind.h>
#include "contracts/queue_rebind_caps.h"
#include "sel4_ipc.h"
#include "boot_info.h"
#include "system_desc.h"
#include <sel4/sel4.h>

static uint32_t rebind(aos_input_service_t *service, uint64_t badge,
                       const input_virt_rebind_req_t *request)
{
    uint32_t status=aos_input_rebind_validate(service,badge,request,sizeof(*request));
    if (status!=AOS_INPUT_OK) return status;
    seL4_CPtr frame=AOS_QUEUE_SERVICE_FRAME_BASE+request->client;
    uintptr_t address=AOS_INPUT_SHMEM_VA+request->client*AOS_INPUT_FRAME_SIZE;
    if (seL4_CNode_Delete(AOS_QUEUE_SERVICE_CNODE,frame,AOS_QUEUE_SERVICE_CNODE_BITS)!=seL4_NoError)
        return AOS_INPUT_BAD_REQUEST;
    if (seL4_Untyped_Retype(AOS_QUEUE_SERVICE_RECEIVE,seL4_ARCH_LargePageObject,0u,
        AOS_QUEUE_SERVICE_CNODE,0u,0u,frame,1u)!=seL4_NoError) return AOS_INPUT_BAD_REQUEST;
    if (seL4_ARCH_Page_Map(frame,AOS_QUEUE_SERVICE_VSPACE,address,seL4_AllRights,
        seL4_ARM_Default_VMAttributes)!=seL4_NoError) status=AOS_INPUT_BAD_REQUEST;
    else status=aos_input_rebind_commit(service,badge,request,sizeof(*request),(void *)address);
    if (status!=AOS_INPUT_OK)
        (void)seL4_CNode_Delete(AOS_QUEUE_SERVICE_CNODE,frame,AOS_QUEUE_SERVICE_CNODE_BITS);
    return status;
}

void pd_main(seL4_CPtr endpoint, seL4_CPtr nameserver)
{
    (void)nameserver;
    aos_input_service_t service;
    aos_input_client_region_t *clients[AOS_INPUT_CLIENTS];
    for (unsigned i=0;i<AOS_INPUT_CLIENTS;++i)
        clients[i]=(void *)(AOS_INPUT_SHMEM_VA+i*AOS_INPUT_FRAME_SIZE);
    if (aos_input_service_init(&service,(void *)AOS_INPUT_FRONTEND_VA,clients,3)!=0)
        for (;;) { seL4_Word badge; seL4_Wait(PD_CNODE_SLOT_INPUT_WAIT,&badge); }
    for (;;) {
        uint32_t ready;
        unsigned progress=aos_input_pump(&service,&ready);
        for (unsigned i=0;i<AOS_INPUT_CLIENTS;++i)
            if (ready & (1u<<i)) seL4_Signal(PD_CNODE_SLOT_INPUT_PEER_NOTIFY+i);
        if (progress) seL4_Signal(PD_CNODE_SLOT_INPUT_PEER_NOTIFY+AOS_INPUT_CLIENTS);
        if (progress) continue;
        /* The event notification is bound to this TCB. Receive on the control
         * endpoint handles either event wakes or a bounded rebind request. */
        (void)seL4_CNode_Delete(AOS_QUEUE_SERVICE_CNODE,AOS_QUEUE_SERVICE_RECEIVE,
            AOS_QUEUE_SERVICE_CNODE_BITS);
        seL4_SetCapReceivePath(AOS_QUEUE_SERVICE_CNODE,AOS_QUEUE_SERVICE_RECEIVE,
            AOS_QUEUE_SERVICE_CNODE_BITS);
        seL4_Word badge=0;
#ifdef CONFIG_KERNEL_MCS
        seL4_MessageInfo_t info=seL4_Recv(endpoint,&badge,AGENTOS_IPC_REPLY_CAP);
#else
        seL4_MessageInfo_t info=seL4_Recv(endpoint,&badge);
#endif
        if (badge && !(badge & ~((1u<<(AOS_INPUT_CLIENTS+1u))-1u))) continue;
        sel4_msg_t message={0}, reply={.opcode=SEL4_ERR_OK,
            .length=sizeof(input_virt_rebind_reply_t)};
        input_virt_rebind_req_t request={0};
        uint32_t status=AOS_INPUT_BAD_REQUEST;
        if (seL4_MessageInfo_get_label(info)==INPUT_VIRT_OP_REBIND &&
            seL4_MessageInfo_get_length(info)==_SEL4_MR_COUNT &&
            seL4_MessageInfo_get_extraCaps(info)==1u &&
            seL4_MessageInfo_get_capsUnwrapped(info)==0u) {
            _sel4_mrs_to_msg(&message);
            if (message.opcode==INPUT_VIRT_OP_REBIND && message.length==sizeof(request)) {
                __builtin_memcpy(&request,message.data,sizeof(request));
                status=rebind(&service,badge,&request);
            }
        }
        input_virt_rebind_reply_t result={status,INPUT_VIRT_REBIND_VERSION,request.generation};
        __builtin_memcpy(reply.data,&result,sizeof(result));
        _sel4_msg_to_mrs(&reply);
        bool success=status==AOS_INPUT_OK;
        seL4_SetCap(0,success ? AOS_QUEUE_SERVICE_FRAME_BASE+request.client : seL4_CapNull);
        seL4_MessageInfo_t response=seL4_MessageInfo_new(SEL4_ERR_OK,0u,success ? 1u : 0u,_SEL4_MR_COUNT);
#ifdef CONFIG_KERNEL_MCS
        seL4_Send(AGENTOS_IPC_REPLY_CAP,response);
#else
        seL4_Reply(response);
#endif
        seL4_SetCap(0,seL4_CapNull);
    }
}
