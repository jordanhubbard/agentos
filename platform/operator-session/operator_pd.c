#include "sel4_ipc.h"
#include "serial_virt_client.h"
#include <platform/operator_session.h>
#include <platform/serial_virt_layout.h>
#include <platform/operator_isolation_probe.h>

static aos_operator_session_t session;
void pd_main(seL4_CPtr endpoint, seL4_CPtr nameserver)
{
    (void)endpoint; (void)nameserver;
    aos_serial_channel_t channel = aos_serial_channel_at(AOS_SERIAL_SHMEM_VA +
        SERIAL_VIRT_OPERATOR_CLIENT * AOS_SERIAL_FRAME_SIZE);
    const aos_inspect_snapshot_t *snapshot = (const void *)AOS_INSPECT_BOOT_VA;
#ifdef AGENTOS_OPERATOR_TEST
    if (serial_virt_client_attach(0, SERIAL_VIRT_ROLE_VMM) ||
        serial_virt_client_attach(0, SERIAL_VIRT_ROLE_OPERATOR) ||
        serial_virt_client_attach(SERIAL_VIRT_OPERATOR_CLIENT, SERIAL_VIRT_ROLE_FRONTEND))
        for (;;) { seL4_Word badge; seL4_Wait(PD_CNODE_SLOT_OPERATOR_WAIT, &badge); }
#endif
    if (aos_inspect_validate(snapshot) != AOS_INSPECT_OK ||
        !serial_virt_client_attach(SERIAL_VIRT_OPERATOR_CLIENT, SERIAL_VIRT_ROLE_OPERATOR))
        for (;;) { seL4_Word badge; seL4_Wait(PD_CNODE_SLOT_OPERATOR_WAIT, &badge); }
    __atomic_store_n(&channel.meta->guest_state, 0u, __ATOMIC_RELEASE);
#ifdef AGENTOS_OPERATOR_ISOLATION_PROBE
    volatile uint8_t *forbidden = (volatile uint8_t *)(uintptr_t)AOS_OPERATOR_PROBE_ADDRESS;
    if (AOS_OPERATOR_PROBE_WRITE) *forbidden = 0x5a;
    else { volatile uint8_t value = *forbidden; (void)value; }
    /* A permitted access must never produce a success marker. */
    for (;;) { seL4_Word badge; seL4_Wait(PD_CNODE_SLOT_OPERATOR_WAIT, &badge); }
#endif
    for (;;) {
        int progress = aos_operator_session_pump(&session, snapshot,
            &channel.to_guest, &channel.from_guest);
        if (progress > 0) {
            seL4_Signal(PD_CNODE_SLOT_SERIAL_VIRT_NOTIFY);
            seL4_Yield();
        } else {
            seL4_Word badge;
            seL4_Wait(PD_CNODE_SLOT_OPERATOR_WAIT, &badge);
        }
    }
}
