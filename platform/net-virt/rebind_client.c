#include <platform/net_rebind.h>
#include "contracts/guest_queue_caps.h"
#include "contracts/guest_ram_caps.h"
#include "sel4_ipc.h"
#include "system_desc.h"
#include "boot_info.h"

bool aos_net_virt_rebind_with_info(uint32_t client, uint32_t generation,
                                  net_virt_rebind_reply_t *attachment)
{
    if (client >= AOS_NET_GUEST_CLIENTS || !generation) return false;
    const seL4_CPtr frame = AOS_GUEST_QUEUE_FRAME_BASE + AOS_GUEST_QUEUE_NET;
    net_virt_rebind_req_t rebind = {NET_VIRT_REBIND_VERSION, client, generation};
    sel4_msg_t request = {.opcode = NET_VIRT_OP_REBIND, .length = sizeof(rebind)};
    __builtin_memcpy(request.data, &rebind, sizeof(rebind));
    seL4_SetCapReceivePath(AOS_GUEST_RAM_SELF_CNODE, frame, AOS_GUEST_RAM_CNODE_BITS);
    seL4_SetCap(0, AOS_GUEST_QUEUE_POOL_BASE + AOS_GUEST_QUEUE_NET);
    _sel4_msg_to_mrs(&request);
    seL4_MessageInfo_t info = seL4_Call(PD_CNODE_SLOT_NET_VIRT_EP,
        seL4_MessageInfo_new(request.opcode, 0u, 1u, _SEL4_MR_COUNT));
    seL4_SetCap(0, seL4_CapNull);
    seL4_SetCapReceivePath(seL4_CapNull, 0u, 0u);
    if (seL4_MessageInfo_get_length(info) != _SEL4_MR_COUNT ||
        seL4_MessageInfo_get_label(info) != SEL4_ERR_OK ||
        seL4_MessageInfo_get_extraCaps(info) != 1u ||
        seL4_MessageInfo_get_capsUnwrapped(info) != 0u) return false;
    sel4_msg_t reply;
    net_virt_rebind_reply_t result;
    _sel4_mrs_to_msg(&reply);
    if (reply.opcode != SEL4_ERR_OK || reply.length != sizeof(result)) return false;
    __builtin_memcpy(&result, reply.data, sizeof(result));
    if (!aos_net_rebind_reply_valid(&result, sizeof(result), generation)) return false;
    if (seL4_ARCH_Page_Map(frame, AOS_GUEST_RAM_VMM_VSPACE,
        aos_net_rebind_queue_va(client), seL4_AllRights,
        seL4_ARM_Default_VMAttributes) != seL4_NoError) return false;
    if (attachment) *attachment = result;
    return true;
}

bool aos_net_virt_rebind(uint32_t client, uint32_t generation)
{
    return aos_net_virt_rebind_with_info(client, generation, NULL);
}
