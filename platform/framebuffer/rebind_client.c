#include <platform/framebuffer_rebind_client.h>
#include <platform/framebuffer.h>
#include "contracts/guest_graphics_caps.h"
#include "contracts/guest_queue_caps.h"
#include "contracts/guest_ram_caps.h"
#include "sel4_ipc.h"
#include "system_desc.h"
#include "boot_info.h"
_Static_assert(AOS_GUEST_GRAPHICS_QUEUE_FRAME>=AOS_GUEST_QUEUE_FRAME_BASE+AOS_GUEST_QUEUE_POOL_COUNT,
    "graphics queue frame must not overlap other queue frames");

bool aos_fb_virt_rebind_exchange(uint32_t op,const fb_rebind_req_t *q,fb_rebind_reply_t *out)
{
    if (!q || !out || q->version!=FB_REBIND_VERSION || q->client>=AOS_FB_CLIENTS ||
        !q->generation || op<FB_REBIND_STAGE || op>FB_REBIND_ABORT ||
        (op==FB_REBIND_STAGE ? q->index>=FB_REBIND_FRAMES : q->index!=0)) return false;
    sel4_msg_t request={.opcode=op,.length=sizeof(*q)};
    __builtin_memcpy(request.data,q,sizeof(*q));
    seL4_SetCapReceivePath(AOS_GUEST_RAM_SELF_CNODE,AOS_GUEST_GRAPHICS_QUEUE_FRAME,
        AOS_GUEST_RAM_CNODE_BITS);
    seL4_SetCap(0,op==FB_REBIND_STAGE ? AOS_GUEST_GRAPHICS_POOL_BASE+q->index : seL4_CapNull);
    _sel4_msg_to_mrs(&request);
    seL4_MessageInfo_t info=seL4_Call(PD_CNODE_SLOT_FB_REBIND_EP,
        seL4_MessageInfo_new(op,0u,op==FB_REBIND_STAGE ? 1u : 0u,_SEL4_MR_COUNT));
    seL4_SetCap(0,seL4_CapNull);
    seL4_SetCapReceivePath(seL4_CapNull,0u,0u);
    if (seL4_MessageInfo_get_length(info)!=_SEL4_MR_COUNT ||
        seL4_MessageInfo_get_label(info)!=SEL4_ERR_OK) return false;
    sel4_msg_t reply;
    fb_rebind_reply_t result;
    _sel4_mrs_to_msg(&reply);
    if (reply.opcode!=SEL4_ERR_OK || reply.length!=sizeof(result)) return false;
    __builtin_memcpy(&result,reply.data,sizeof(result));
    if (!aos_fb_rebind_reply_valid(op,q,&result,sizeof(result),
        seL4_MessageInfo_get_extraCaps(info),seL4_MessageInfo_get_capsUnwrapped(info))) return false;
    *out=result;
    return true;
}
bool aos_fb_virt_map_queue(uint32_t client)
{
    return client<AOS_FB_CLIENTS &&
        seL4_ARCH_Page_Map(AOS_GUEST_GRAPHICS_QUEUE_FRAME,AOS_GUEST_RAM_VMM_VSPACE,
            AOS_FB_SHMEM_VA+client*AOS_FB_CLIENT_STRIDE,seL4_AllRights,
            seL4_ARM_Default_VMAttributes)==seL4_NoError;
}
