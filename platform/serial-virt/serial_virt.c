/* Separate serial virtualizer PD. No device frame or hardware IRQ authority.
 * Root mappings and client adapters must be present before this PD is booted. */
#include "agentos.h"
#include "sel4_ipc.h"
#include "system_desc.h"
#include <platform/serial_virt_service.h>
#include "contracts/queue_rebind_caps.h"
#include "boot_info.h"

uintptr_t log_drain_rings_vaddr;
static aos_serial_virt_service_t service;
static uint32_t fault_reported;
static uint32_t input_reported, output_reported;

static uint32_t rebind_queue(uint64_t badge, const serial_virt_rebind_req_t *req)
{
    uint32_t status = aos_serial_virt_rebind_validate(&service, badge, req, sizeof(*req));
    if (status != SERIAL_VIRT_OK) return status;
    seL4_CPtr frame = AOS_QUEUE_SERVICE_FRAME_BASE + req->client;
    /* Validate retirement before deleting any old service mapping. */
    if (seL4_CNode_Delete(AOS_QUEUE_SERVICE_CNODE, frame,
            AOS_QUEUE_SERVICE_CNODE_BITS) != seL4_NoError) return SERIAL_VIRT_ERR_RESOURCE;
    if (seL4_Untyped_Retype(AOS_QUEUE_SERVICE_RECEIVE, seL4_ARCH_LargePageObject,
            0u, AOS_QUEUE_SERVICE_CNODE, 0u, 0u, frame, 1u) != seL4_NoError)
        return SERIAL_VIRT_ERR_RESOURCE;
    uintptr_t va = AOS_SERIAL_SHMEM_VA + req->client * AOS_SERIAL_FRAME_SIZE;
    if (seL4_ARCH_Page_Map(frame, AOS_QUEUE_SERVICE_VSPACE, va, seL4_AllRights,
            seL4_ARM_Default_VMAttributes) != seL4_NoError) status = SERIAL_VIRT_ERR_RESOURCE;
    else status = aos_serial_virt_rebind_commit(&service, badge, req, sizeof(*req),
        aos_serial_channel_at(va));
    if (status != SERIAL_VIRT_OK)
        (void)seL4_CNode_Delete(AOS_QUEUE_SERVICE_CNODE, frame, AOS_QUEUE_SERVICE_CNODE_BITS);
    return status;
}

static void service_queues(void)
{
    /* One full-capacity snapshot per direction handles all currently queued
     * bytes. New producer activity signals again; backpressure is retried
     * when the consumer signals that it freed space. No unbounded rescans. */
    aos_serial_virt_result_t result = aos_serial_virt_service_pump(
        &service, AOS_SERIAL_TX_CAPACITY);
    if (result.input_clients & 3u & ~input_reported) {
        agentos_log_info("serial_virt", "frontend input delivered to VMM queue");
        input_reported |= result.input_clients;
    }
    if (result.output_clients & 3u & ~output_reported) {
        agentos_log_info("serial_virt", "VMM output delivered to frontend queue");
        output_reported |= result.output_clients;
    }
    if (result.input_clients & 4u & ~input_reported)
        agentos_log_info("serial_virt", "operator input transferred");
    if (result.output_clients & 4u & ~output_reported)
        agentos_log_info("serial_virt", "operator output transferred");
    input_reported |= result.input_clients;
    output_reported |= result.output_clients;
    if (result.invalid_clients & ~fault_reported) {
        agentos_log_info("serial_virt", "malformed client queue rejected");
        fault_reported |= result.invalid_clients;
    }
#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_X86_FIRMWARE_RESET)
    if (result.wake_vmm & 1u)
        seL4_Signal(PD_CNODE_SLOT_SERIAL_PRIMARY_NOTIFY);
#endif
#if defined(AGENTOS_GUEST_SECONDARY)
    if (result.wake_vmm & 2u)
        seL4_Signal(PD_CNODE_SLOT_SERIAL_SECONDARY_NOTIFY);
#endif
#if defined(__aarch64__)
    if (result.wake_vmm & SERIAL_VIRT_OPERATOR_WAKE_BADGE)
        seL4_Signal(PD_CNODE_SLOT_SERIAL_OPERATOR_NOTIFY);
#endif
}

void pd_main(seL4_CPtr endpoint, seL4_CPtr nameserver)
{
    (void)nameserver;
    for (uint32_t i = 0; i < AOS_SERIAL_CLIENTS; i++) {
        service.guest[i] = aos_serial_channel_at(AOS_SERIAL_SHMEM_VA +
                                               i * AOS_SERIAL_FRAME_SIZE);
        service.frontend[i] = aos_serial_channel_at(AOS_SERIAL_SHMEM_VA +
            AOS_SERIAL_FRONTEND_FRAME * AOS_SERIAL_FRAME_SIZE +
            i * AOS_SERIAL_FRONTEND_STRIDE);
    }
    agentos_log_info("serial_virt", "READY: isolated serial queue service v4");
    for (;;) {
        (void)seL4_CNode_Delete(AOS_QUEUE_SERVICE_CNODE, AOS_QUEUE_SERVICE_RECEIVE,
            AOS_QUEUE_SERVICE_CNODE_BITS);
        seL4_SetCapReceivePath(AOS_QUEUE_SERVICE_CNODE, AOS_QUEUE_SERVICE_RECEIVE,
            AOS_QUEUE_SERVICE_CNODE_BITS);
        seL4_Word badge = 0;
#ifdef CONFIG_KERNEL_MCS
        seL4_MessageInfo_t info = seL4_Recv(endpoint, &badge, AGENTOS_IPC_REPLY_CAP);
#else
        seL4_MessageInfo_t info = seL4_Recv(endpoint, &badge);
#endif
        if (serial_virt_service_notification(badge)) {
            service_queues();
            continue;
        }
        seL4_Word label = seL4_MessageInfo_get_label(info);
        sel4_msg_t request = {0}, reply = {0};
        /* A short IPC must not reuse stale message registers from an earlier
         * caller. The canonical sel4_call wrapper sends this exact size. */
        if ((label == SERIAL_VIRT_OP_ATTACH || label == SERIAL_VIRT_OP_DETACH ||
             label == SERIAL_VIRT_OP_REBIND) &&
            seL4_MessageInfo_get_length(info) == _SEL4_MR_COUNT &&
            seL4_GetMR(0) == label && seL4_GetMR(1) <= SEL4_MSG_DATA_BYTES)
            _sel4_mrs_to_msg(&request);
        serial_virt_attach_req_t attach = {0};
        uint32_t status = SERIAL_VIRT_ERR_PROTOCOL;
        if ((label == SERIAL_VIRT_OP_ATTACH || label == SERIAL_VIRT_OP_DETACH) &&
            request.length == sizeof(attach) && seL4_MessageInfo_get_extraCaps(info) == 0u) {
            __builtin_memcpy(&attach, request.data, sizeof(attach));
            status = label == SERIAL_VIRT_OP_ATTACH ?
                aos_serial_virt_attach(&service, badge, &attach, request.length) :
                aos_serial_virt_detach(&service, badge, &attach, request.length);
            if (label == SERIAL_VIRT_OP_DETACH && status == SERIAL_VIRT_OK)
                agentos_log_info("serial_virt", "DETACH guest queues released");
        }
        rep_u32(&reply, 0, status);
        rep_u32(&reply, 4, SERIAL_VIRT_CONTRACT_VERSION);
        reply.length = sizeof(serial_virt_attach_reply_t);
        bool rebound = false;
        if (label == SERIAL_VIRT_OP_REBIND) {
            serial_virt_rebind_req_t rebind = {0};
            if (request.length == sizeof(rebind) &&
                seL4_MessageInfo_get_extraCaps(info) == 1u &&
                seL4_MessageInfo_get_capsUnwrapped(info) == 0u) {
                __builtin_memcpy(&rebind, request.data, sizeof(rebind));
                status = rebind_queue(badge, &rebind);
            }
            rebound = status == SERIAL_VIRT_OK;
            rep_u32(&reply, 0, status);
            rep_u32(&reply, 4, SERIAL_VIRT_REBIND_VERSION);
            rep_u32(&reply, 8, rebind.generation);
            reply.length = 12u;
            if (rebound) seL4_SetCap(0, AOS_QUEUE_SERVICE_FRAME_BASE + rebind.client);
        }
        reply.opcode = SEL4_ERR_OK;
        _sel4_msg_to_mrs(&reply);
        seL4_MessageInfo_t result = seL4_MessageInfo_new(
            reply.opcode, 0, rebound ? 1u : 0u, _SEL4_MR_COUNT);
#ifdef CONFIG_KERNEL_MCS
        seL4_Send(AGENTOS_IPC_REPLY_CAP, result);
#else
        seL4_Reply(result);
#endif
        seL4_SetCap(0, seL4_CapNull);
        /* Qualification-only wake from the real service capability. The
         * Intel userspace result requires this notification to be handled. */
#if defined(AGENTOS_X86_USERSPACE_PROOF)
        if (label == SERIAL_VIRT_OP_ATTACH && status == SERIAL_VIRT_OK &&
            attach.role == SERIAL_VIRT_ROLE_VMM && attach.client == 0)
            seL4_Signal(PD_CNODE_SLOT_SERIAL_PRIMARY_NOTIFY);
#endif
        if (status == SERIAL_VIRT_OK) service_queues();
    }
}
