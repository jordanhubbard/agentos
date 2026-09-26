#include <platform/x86_control.h>
#include "sel4_ipc.h"
#include "system_desc.h"
#include "contracts/guest_contract.h"
#include "contracts/serial_virt_contract.h"
#include "contracts/blk_virt_contract.h"
#include "contracts/net_virt_contract.h"
#include "contracts/x86_vtx_proof.h"
#ifdef AGENTOS_GUEST_INPUT
#include <platform/input.h>
#define X86_INPUT_WAKE_MASK AOS_INPUT_VMM_WAKE_BADGE
#else
#define X86_INPUT_WAKE_MASK 0u
#endif

static const seL4_Word control_wakes = SERIAL_VIRT_VMM_WAKE_BADGE |
    BLK_VIRT_VMM_WAKE_BADGE | NET_VIRT_VMM_WAKE_BADGE | X86_INPUT_WAKE_MASK;
#ifdef AGENTOS_X86_LIFECYCLE_WITNESS
volatile aos_x86_lifecycle_witness_t aos_x86_control_witness = {
    .magic = UINT64_C(0x414f534c4354524c), .version = UINT64_C(0x4c49464557495431),
};
#endif
#ifdef AGENTOS_X86_USERSPACE_PROOF
extern bool aos_x86_lifecycle_ack;
extern bool aos_x86_lifecycle_boot_ack;
#endif

static bool initializing_receive(seL4_Word *wake_badge, bool wait)
{
    if (!wake_badge) return false;
    *wake_badge = 0;
    seL4_Word badge = 0;
#ifdef CONFIG_KERNEL_MCS
    if (wait) (void)seL4_Recv(PD_CNODE_SLOT_SELF_EP, &badge, AGENTOS_IPC_REPLY_CAP);
    else (void)seL4_NBRecv(PD_CNODE_SLOT_SELF_EP, &badge, AGENTOS_IPC_REPLY_CAP);
#else
    if (wait) (void)seL4_Recv(PD_CNODE_SLOT_SELF_EP, &badge);
    else (void)seL4_NBRecv(PD_CNODE_SLOT_SELF_EP, &badge);
#endif
    const seL4_Word wakes = control_wakes;
    if (badge & wakes) {
        if (badge & ~wakes) return false;
        *wake_badge = badge;
    } else if (badge) {
        /* Reject all calls before initialization completes, including malformed
         * frames. Do not decode, mutate resources or leave a caller blocked. */
        const sel4_msg_t reply = {.opcode = GUEST_ERR_NOT_READY};
        _sel4_msg_to_mrs(&reply);
        seL4_MessageInfo_t info = seL4_MessageInfo_new(reply.opcode, 0, 0, _SEL4_MR_COUNT);
#ifdef CONFIG_KERNEL_MCS
        seL4_Send(AGENTOS_IPC_REPLY_CAP, info);
#else
        seL4_Reply(info);
#endif
    } else return !wait;
    return true;
}

bool aos_x86_control_wait_initializing(seL4_Word *wake_badge)
{ return initializing_receive(wake_badge, true); }

bool aos_x86_control_poll_initializing(seL4_Word *wake_badge)
{ return initializing_receive(wake_badge, false); }

enum aos_x86_control_result aos_x86_control_step(
    const aos_guest_vmm_runtime_t *runtime,
    void (*wake)(seL4_Word, void *), void *context)
{
    if (!runtime || !runtime->state || !runtime->started)
        return AOS_X86_CONTROL_ERROR;
    seL4_Word badge = 0;
    bool running = *runtime->state == GUEST_STATE_RUNNING;
    AOS_X86_CONTROL_STAGE(1);
    seL4_MessageInfo_t info;
#ifdef CONFIG_KERNEL_MCS
    info = running ? seL4_NBRecv(PD_CNODE_SLOT_SELF_EP, &badge, AGENTOS_IPC_REPLY_CAP)
                   : seL4_Recv(PD_CNODE_SLOT_SELF_EP, &badge, AGENTOS_IPC_REPLY_CAP);
#else
    info = running ? seL4_NBRecv(PD_CNODE_SLOT_SELF_EP, &badge)
                   : seL4_Recv(PD_CNODE_SLOT_SELF_EP, &badge);
#endif
    AOS_X86_CONTROL_STAGE(2);
#ifdef AGENTOS_X86_LIFECYCLE_WITNESS
    aos_x86_control_witness.badge = badge;
    aos_x86_control_witness.count++;
    aos_x86_control_witness.state = *runtime->state;
#endif
    const seL4_Word wakes = control_wakes;
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
#ifdef AGENTOS_X86_LIFECYCLE_WITNESS
            aos_x86_control_witness.opcode = request.opcode;
#endif
            AOS_X86_CONTROL_STAGE(3);
#ifdef AGENTOS_X86_USERSPACE_PROOF
            if (request.opcode == AOS_X86_LIFECYCLE_ACK &&
                badge == (((seL4_Word)SVC_ID_GUEST_VMM_PRIMARY << 48) |
                          ((seL4_Word)AOS_X86_LIFECYCLE_PROBE_INDEX << 32)) &&
                *runtime->state == GUEST_STATE_DEAD &&
                request.length == 4u && msg_u32(&request, 0u) == AOS_X86_USERSPACE_PASS) {
                aos_x86_lifecycle_ack = true;
                reply.opcode = GUEST_OK;
            } else if (request.opcode == AOS_X86_LIFECYCLE_BOOT_ACK &&
                badge == (((seL4_Word)SVC_ID_GUEST_VMM_PRIMARY << 48) |
                          ((seL4_Word)AOS_X86_LIFECYCLE_PROBE_INDEX << 32)) &&
                *runtime->state == GUEST_STATE_RUNNING &&
                request.length == 4u && msg_u32(&request, 0u) == AOS_X86_USERSPACE_PASS) {
                aos_x86_lifecycle_boot_ack = true;
                reply.opcode = GUEST_OK;
            } else
#endif
            (void)aos_guest_vmm_lifecycle_rpc(&request, &reply, runtime);
        }
        AOS_X86_CONTROL_STAGE(4);
#ifdef AGENTOS_X86_LIFECYCLE_WITNESS
        aos_x86_control_witness.status = reply.opcode;
        aos_x86_control_witness.state = *runtime->state;
#endif
#ifdef AGENTOS_X86_LIFECYCLE_TRACE
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
        AOS_X86_CONTROL_STAGE(5);
#ifdef CONFIG_KERNEL_MCS
        seL4_Send(AGENTOS_IPC_REPLY_CAP, info);
#else
        seL4_Reply(info);
#endif
        AOS_X86_CONTROL_STAGE(6);
    } else if (!running) {
        /* Production endpoint grants are always badged. A blocking receive
         * cannot be the empty NBRecv case. */
        return AOS_X86_CONTROL_ERROR;
    }
    return *runtime->state == GUEST_STATE_RUNNING ? AOS_X86_CONTROL_RUNNING
                                                : AOS_X86_CONTROL_STOPPED;
}
