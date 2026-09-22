#include "rebind_endpoint.h"
#include <platform/framebuffer_rebind.h>
#include "contracts/queue_rebind_caps.h"
#include "sel4_ipc.h"
#include "boot_info.h"

static aos_fb_rebind_t pending[AOS_FB_CLIENTS];
static seL4_CPtr frame_slot(unsigned client,unsigned index)
{
    return AOS_QUEUE_SERVICE_FRAME_BASE+client*FB_REBIND_FRAMES+index;
}
static uint32_t execute(aos_fb_client_t clients[AOS_FB_CLIENTS],uint64_t badge,
                        uint32_t op,const fb_rebind_req_t *q)
{
    if (q->client>=AOS_FB_CLIENTS) return FB_REBIND_DENIED;
    aos_fb_client_t *c=&clients[q->client];
    aos_fb_rebind_t *s=&pending[q->client];
    uint32_t status=aos_fb_rebind_validate(c,s,badge,op,q,sizeof(*q));
    if (status!=FB_REBIND_OK) return status;
    uintptr_t queue=AOS_FB_SHMEM_VA+q->client*AOS_FB_CLIENT_STRIDE;
    uintptr_t arena=AOS_FB_ARENA_VA+q->client*AOS_FB_ARENA_BYTES;
    if (op==FB_REBIND_COMMIT)
        return aos_fb_rebind_commit(c,s,badge,q,(void *)queue,(void *)arena);
    if (op==FB_REBIND_ABORT) {
        for (unsigned i=0;i<s->next_frame;i++)
            if (seL4_CNode_Delete(AOS_QUEUE_SERVICE_CNODE,frame_slot(q->client,i),
                AOS_QUEUE_SERVICE_CNODE_BITS)!=seL4_NoError) return FB_REBIND_RESOURCE;
        return aos_fb_rebind_aborted(c,s,badge,q);
    }
    seL4_CPtr frame=frame_slot(q->client,q->index);
    uintptr_t address=q->index ? arena+(q->index-1u)*AOS_FB_CLIENT_STRIDE : queue;
    if (seL4_CNode_Delete(AOS_QUEUE_SERVICE_CNODE,frame,AOS_QUEUE_SERVICE_CNODE_BITS)!=seL4_NoError ||
        seL4_Untyped_Retype(AOS_QUEUE_SERVICE_RECEIVE,seL4_ARCH_LargePageObject,0u,
            AOS_QUEUE_SERVICE_CNODE,0u,0u,frame,1u)!=seL4_NoError) return FB_REBIND_RESOURCE;
    if (seL4_ARCH_Page_Map(frame,AOS_QUEUE_SERVICE_VSPACE,address,seL4_AllRights,
        seL4_ARM_Default_VMAttributes)!=seL4_NoError) status=FB_REBIND_RESOURCE;
    else status=aos_fb_rebind_staged(c,s,badge,q);
    if (status!=FB_REBIND_OK)
        (void)seL4_CNode_Delete(AOS_QUEUE_SERVICE_CNODE,frame,AOS_QUEUE_SERVICE_CNODE_BITS);
    return status;
}
void aos_fb_rebind_receive(aos_fb_client_t clients[AOS_FB_CLIENTS],seL4_CPtr endpoint)
{
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
    /* Bound notification badges occupy only the low three bits. */
    if (badge && !(badge & ~((1u<<(AOS_FB_CLIENTS+1u))-1u))) return;
    uint32_t op=(uint32_t)seL4_MessageInfo_get_label(info),status=FB_REBIND_BAD_REQUEST;
    sel4_msg_t request={0},reply={.opcode=SEL4_ERR_OK,.length=sizeof(fb_rebind_reply_t)};
    fb_rebind_req_t q={0};
    if (op>=FB_REBIND_STAGE && op<=FB_REBIND_ABORT &&
        seL4_MessageInfo_get_length(info)==_SEL4_MR_COUNT &&
        seL4_MessageInfo_get_extraCaps(info)==(op==FB_REBIND_STAGE ? 1u : 0u) &&
        !seL4_MessageInfo_get_capsUnwrapped(info)) {
        _sel4_mrs_to_msg(&request);
        if (request.opcode==op && request.length==sizeof(q)) {
            __builtin_memcpy(&q,request.data,sizeof(q));
            status=execute(clients,badge,op,&q);
        }
    }
    fb_rebind_reply_t result={status,FB_REBIND_VERSION,q.generation,q.index};
    __builtin_memcpy(reply.data,&result,sizeof(result));
    _sel4_msg_to_mrs(&reply);
    unsigned cap=status==FB_REBIND_OK && op==FB_REBIND_STAGE && !q.index;
    seL4_SetCap(0,cap ? frame_slot(q.client,0) : seL4_CapNull);
    seL4_MessageInfo_t response=seL4_MessageInfo_new(SEL4_ERR_OK,0u,cap,_SEL4_MR_COUNT);
#ifdef CONFIG_KERNEL_MCS
    seL4_Send(AGENTOS_IPC_REPLY_CAP,response);
#else
    seL4_Reply(response);
#endif
    seL4_SetCap(0,seL4_CapNull);
}
