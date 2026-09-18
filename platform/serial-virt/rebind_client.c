#include <platform/serial_rebind.h>
#include <platform/serial_virt_layout.h>
#include "contracts/guest_queue_caps.h"
#include "contracts/guest_ram_caps.h"
#include "serial_virt_client.h"
#include "boot_info.h"

bool aos_serial_virt_rebind(uint32_t client, uint32_t generation)
{
    if (client >= 2u || !generation) return false;
    const seL4_CPtr frame = AOS_GUEST_QUEUE_FRAME_BASE + AOS_GUEST_QUEUE_SERIAL;
    serial_virt_rebind_req_t rebind = {SERIAL_VIRT_REBIND_VERSION, client, generation};
    sel4_msg_t request = {.opcode = SERIAL_VIRT_OP_REBIND, .length = sizeof(rebind)};
    __builtin_memcpy(request.data, &rebind, sizeof(rebind));
    seL4_SetCapReceivePath(AOS_GUEST_RAM_SELF_CNODE, frame, AOS_GUEST_RAM_CNODE_BITS);
    seL4_SetCap(0, AOS_GUEST_QUEUE_POOL_BASE + AOS_GUEST_QUEUE_SERIAL);
    _sel4_msg_to_mrs(&request);
    seL4_MessageInfo_t info = seL4_Call(PD_CNODE_SLOT_SERIAL_VIRT_EP,
        seL4_MessageInfo_new(request.opcode, 0u, 1u, _SEL4_MR_COUNT));
    seL4_SetCap(0, seL4_CapNull);
    seL4_SetCapReceivePath(seL4_CapNull, 0u, 0u);
    if (seL4_MessageInfo_get_length(info) != _SEL4_MR_COUNT ||
        seL4_MessageInfo_get_label(info) != SEL4_ERR_OK ||
        seL4_MessageInfo_get_extraCaps(info) != 1u ||
        seL4_MessageInfo_get_capsUnwrapped(info) != 0u) return false;
    sel4_msg_t reply;
    _sel4_mrs_to_msg(&reply);
    if (reply.opcode != SEL4_ERR_OK || reply.length != 12u ||
        msg_u32(&reply, 0u) != SERIAL_VIRT_OK ||
        msg_u32(&reply, 4u) != SERIAL_VIRT_REBIND_VERSION ||
        msg_u32(&reply, 8u) != generation) return false;
    return seL4_ARCH_Page_Map(frame, AOS_GUEST_RAM_VMM_VSPACE,
        AOS_SERIAL_SHMEM_VA + client * AOS_SERIAL_FRAME_SIZE,
        seL4_AllRights, seL4_ARM_Default_VMAttributes) == seL4_NoError;
}
