#include <platform/serial_virt_service.h>

uint32_t aos_serial_virt_attach(aos_serial_virt_service_t *service,
    uint64_t badge, const serial_virt_attach_req_t *request, uint32_t length)
{
    if (!service || !request || length != sizeof(*request))
        return SERIAL_VIRT_ERR_PROTOCOL;
    if (request->version != SERIAL_VIRT_CONTRACT_VERSION)
        return SERIAL_VIRT_ERR_VERSION;
    if (!serial_virt_authorized(badge, request->client, request->role))
        return SERIAL_VIRT_ERR_AUTHORITY;
    uint8_t *attached = request->role == SERIAL_VIRT_ROLE_VMM ?
        &service->guest_attached[request->client] :
        &service->frontend_attached[request->client];
    if (*attached) return SERIAL_VIRT_ERR_BUSY;
    *attached = 1;
    /* ATTACH grants service access; it never clears a live queue. */
    return SERIAL_VIRT_OK;
}

aos_serial_virt_result_t aos_serial_virt_service_pump(
    aos_serial_virt_service_t *service, uint32_t budget)
{
    aos_serial_virt_result_t result = {0};
    for (uint32_t i = 0; i < AOS_SERIAL_CLIENTS; i++) {
        if (!service->guest_attached[i] || !service->frontend_attached[i]) continue;
        aos_serial_channel_t *guest = &service->guest[i];
        aos_serial_channel_t *front = &service->frontend[i];
        uint32_t state = __atomic_load_n(&guest->meta->guest_state, __ATOMIC_ACQUIRE);
        __atomic_store_n(&front->meta->guest_state, state, __ATOMIC_RELEASE);
        __atomic_store_n(&front->meta->attached, 1u, __ATOMIC_RELEASE);
        uint32_t output = 0, input = 0;
        if (aos_serial_virt_transfer(&guest->from_guest, &front->from_guest,
                                     budget, &output) != AOS_SERIAL_PUMP_OK)
            result.invalid_clients |= 1u << i;
        if (aos_serial_virt_transfer(&front->to_guest, &guest->to_guest,
                                     budget, &input) != AOS_SERIAL_PUMP_OK)
            result.invalid_clients |= 1u << i;
        if (input || output) {
            result.wake_vmm |= 1u << i;
            result.frontend_changed |= 1u << i;
        }
        result.bytes += input + output;
    }
    return result;
}
