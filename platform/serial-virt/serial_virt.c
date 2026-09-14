/* Separate serial virtualizer PD. No device frame or hardware IRQ authority.
 * Root mappings and client adapters must be present before this PD is booted. */
#include "agentos.h"
#include "sel4_ipc.h"
#include "serial_log.h"
#include "system_desc.h"
#include <platform/serial_virt_service.h>

uintptr_t log_drain_rings_vaddr;
static aos_serial_virt_service_t service;
static serial_log_t diagnostic = {.ep = PD_CNODE_SLOT_SERIAL_EP};
static uint32_t fault_reported;
static uint32_t input_reported, output_reported;

static void service_queues(void)
{
    /* One full-capacity snapshot per direction handles all currently queued
     * bytes. New producer activity signals again; backpressure is retried
     * when the consumer signals that it freed space. No unbounded rescans. */
    aos_serial_virt_result_t result = aos_serial_virt_service_pump(
        &service, AOS_SERIAL_TX_CAPACITY);
    if (result.input_clients & 3u & ~input_reported) {
        serial_log_puts(&diagnostic, "[serial_virt] frontend input delivered to VMM queue\n");
        input_reported |= result.input_clients;
    }
    if (result.output_clients & 3u & ~output_reported) {
        serial_log_puts(&diagnostic, "[serial_virt] VMM output delivered to frontend queue\n");
        output_reported |= result.output_clients;
    }
    if (result.input_clients & 4u & ~input_reported)
        serial_log_puts(&diagnostic, "[serial_virt] operator input transferred\n");
    if (result.output_clients & 4u & ~output_reported)
        serial_log_puts(&diagnostic, "[serial_virt] operator output transferred\n");
    input_reported |= result.input_clients;
    output_reported |= result.output_clients;
    if (result.invalid_clients & ~fault_reported) {
        serial_log_puts(&diagnostic, "[serial_virt] malformed client queue rejected\n");
        fault_reported |= result.invalid_clients;
    }
#if defined(AGENTOS_GUEST_PRIMARY)
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
    serial_log_puts(&diagnostic, "[serial_virt] READY: isolated serial queue service v2\n");
    for (;;) {
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
        if (label == SERIAL_VIRT_OP_ATTACH &&
            seL4_MessageInfo_get_length(info) == _SEL4_MR_COUNT)
            _sel4_mrs_to_msg(&request);
        serial_virt_attach_req_t attach = {0};
        uint32_t status = SERIAL_VIRT_ERR_PROTOCOL;
        if (label == SERIAL_VIRT_OP_ATTACH && request.length == sizeof(attach)) {
            __builtin_memcpy(&attach, request.data, sizeof(attach));
            status = aos_serial_virt_attach(&service, badge, &attach, request.length);
        }
        rep_u32(&reply, 0, status);
        rep_u32(&reply, 4, SERIAL_VIRT_CONTRACT_VERSION);
        reply.length = sizeof(serial_virt_attach_reply_t);
        reply.opcode = SEL4_ERR_OK;
        _sel4_msg_to_mrs(&reply);
        seL4_MessageInfo_t result = seL4_MessageInfo_new(
            reply.opcode, 0, 0, _SEL4_MR_COUNT);
#ifdef CONFIG_KERNEL_MCS
        seL4_Send(AGENTOS_IPC_REPLY_CAP, result);
#else
        seL4_Reply(result);
#endif
        if (status == SERIAL_VIRT_OK) service_queues();
    }
}
