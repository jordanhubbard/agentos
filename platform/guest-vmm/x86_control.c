#include <platform/x86_control.h>
#include "sel4_ipc.h"
#include "system_desc.h"
#include "contracts/guest_contract.h"
#include "contracts/serial_virt_contract.h"
#include "contracts/blk_virt_contract.h"
#include "contracts/net_virt_contract.h"
#ifdef AGENTOS_X86_USERSPACE_PROOF
#include "contracts/x86_vtx_proof.h"
extern bool aos_x86_lifecycle_ack;
#endif

enum aos_x86_control_result aos_x86_control_step(
    const aos_guest_vmm_runtime_t *runtime,
    void (*wake)(seL4_Word, void *), void *context)
{
    if (!runtime || !runtime->state || !runtime->started)
        return AOS_X86_CONTROL_ERROR;
    seL4_Word badge = 0;
    bool running = *runtime->state == GUEST_STATE_RUNNING;
    seL4_MessageInfo_t info;
#ifdef CONFIG_KERNEL_MCS
    info = running ? seL4_NBRecv(PD_CNODE_SLOT_SELF_EP, &badge, AGENTOS_IPC_REPLY_CAP)
                   : seL4_Recv(PD_CNODE_SLOT_SELF_EP, &badge, AGENTOS_IPC_REPLY_CAP);
#else
    info = running ? seL4_NBRecv(PD_CNODE_SLOT_SELF_EP, &badge)
                   : seL4_Recv(PD_CNODE_SLOT_SELF_EP, &badge);
#endif
    const seL4_Word wakes = SERIAL_VIRT_VMM_WAKE_BADGE |
                           BLK_VIRT_VMM_WAKE_BADGE | NET_VIRT_VMM_WAKE_BADGE;
    if (badge & wakes) {
        if ((badge & ~wakes) || !wake) return AOS_X86_CONTROL_ERROR;
        wake(badge, context);
    } else if (badge) {
        sel4_msg_t request = {0}, reply = {.opcode = GUEST_ERR_PROTOCOL_VIOLATION};
        /* The shared decoder clamps lengths and reads a complete frame.
         * Validate first so stale MRs cannot supply missing arguments. */
        if (seL4_MessageInfo_get_length(info) == _SEL4_MR_COUNT &&
            seL4_MessageInfo_get_extraCaps(info) == 0u &&
            seL4_MessageInfo_get_capsUnwrapped(info) == 0u &&
            seL4_GetMR(0) <= UINT32_MAX &&
            seL4_MessageInfo_get_label(info) == seL4_GetMR(0) &&
            seL4_GetMR(1) <= SEL4_MSG_DATA_BYTES) {
            _sel4_mrs_to_msg(&request);
#ifdef AGENTOS_X86_USERSPACE_PROOF
            if (request.opcode == AOS_X86_LIFECYCLE_ACK &&
                badge == (((seL4_Word)SVC_ID_GUEST_VMM_PRIMARY << 48) |
                          ((seL4_Word)AOS_X86_LIFECYCLE_PROBE_INDEX << 32)) &&
                *runtime->state == GUEST_STATE_DEAD &&
                request.length == 4u && msg_u32(&request, 0u) == AOS_X86_USERSPACE_PASS) {
                aos_x86_lifecycle_ack = true;
                reply.opcode = GUEST_OK;
            } else
#endif
            (void)aos_guest_vmm_lifecycle_rpc(&request, &reply, runtime);
        }
#ifdef AGENTOS_X86_USERSPACE_PROOF
        /* Bounded observation only; root never decides a lifecycle action.
         * Preserve the reply in native memory across this diagnostic IPC. */
        static unsigned traces;
        if (traces < 32u || aos_x86_lifecycle_ack) {
            traces++;
            seL4_SetMR(0, request.opcode);
            seL4_SetMR(1, reply.opcode);
            seL4_SetMR(2, *runtime->state);
            seL4_SetMR(3, *runtime->started);
            seL4_Send(AOS_X86_VTX_REPORT_CAP,
                seL4_MessageInfo_new(AOS_X86_LIFECYCLE_TRACE_LABEL, 0u, 0u, 4u));
        }
#endif
        _sel4_msg_to_mrs(&reply);
        info = seL4_MessageInfo_new(reply.opcode, 0u, 0u, _SEL4_MR_COUNT);
#ifdef CONFIG_KERNEL_MCS
        seL4_Send(AGENTOS_IPC_REPLY_CAP, info);
#else
        seL4_Reply(info);
#endif
    } else if (!running) {
        /* Production endpoint grants are always badged. A blocking receive
         * cannot be the empty NBRecv case. */
        return AOS_X86_CONTROL_ERROR;
    }
    return *runtime->state == GUEST_STATE_RUNNING ? AOS_X86_CONTROL_RUNNING
                                                : AOS_X86_CONTROL_STOPPED;
}
