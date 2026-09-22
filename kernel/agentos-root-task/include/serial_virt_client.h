#ifndef AOS_SERIAL_VIRT_CLIENT_H
#define AOS_SERIAL_VIRT_CLIENT_H
#include <stdbool.h>
#include "sel4_ipc.h"
#include "system_desc.h"
#include "contracts/serial_virt_contract.h"

/* Attach once per role and client; no byte payload ever travels in this IPC. */
static inline bool serial_virt_client_control(uint32_t opcode, uint32_t client, uint32_t role)
{
    serial_virt_attach_req_t attach = {SERIAL_VIRT_CONTRACT_VERSION, client, role};
    sel4_msg_t request = {0}, reply = {0};
    request.opcode = opcode;
    request.length = sizeof(attach);
    __builtin_memcpy(request.data, &attach, sizeof(attach));
    sel4_call(PD_CNODE_SLOT_SERIAL_VIRT_EP, &request, &reply);
    return reply.opcode == SEL4_ERR_OK &&
           reply.length == sizeof(serial_virt_attach_reply_t) &&
           msg_u32(&reply, 0) == SERIAL_VIRT_OK &&
           msg_u32(&reply, 4) == SERIAL_VIRT_CONTRACT_VERSION;
}
static inline bool serial_virt_client_attach(uint32_t client, uint32_t role)
{
    return serial_virt_client_control(SERIAL_VIRT_OP_ATTACH, client, role);
}
static inline bool serial_virt_client_detach(uint32_t client)
{
    return serial_virt_client_control(SERIAL_VIRT_OP_DETACH, client, SERIAL_VIRT_ROLE_VMM);
}
#endif
