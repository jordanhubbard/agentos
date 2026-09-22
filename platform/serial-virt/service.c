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
    uint8_t *attached = request->role != SERIAL_VIRT_ROLE_FRONTEND ?
        &service->guest_attached[request->client] :
        &service->frontend_attached[request->client];
    if (*attached || service->guest_retired[request->client])
        return SERIAL_VIRT_ERR_BUSY;
    *attached = 1;
    /* ATTACH grants service access; it never clears a live queue. */
    return SERIAL_VIRT_OK;
}

uint32_t aos_serial_virt_detach(aos_serial_virt_service_t *service,
    uint64_t badge, const serial_virt_attach_req_t *request, uint32_t length)
{
    if (!service || !request || length != sizeof(*request))
        return SERIAL_VIRT_ERR_PROTOCOL;
    if (request->version != SERIAL_VIRT_CONTRACT_VERSION)
        return SERIAL_VIRT_ERR_VERSION;
    if (request->role != SERIAL_VIRT_ROLE_VMM ||
        !serial_virt_authorized(badge, request->client, request->role))
        return SERIAL_VIRT_ERR_AUTHORITY;
    const uint32_t client = request->client;
    if (service->guest_retired[client]) return SERIAL_VIRT_OK;
    if (!service->frontend[client].meta) return SERIAL_VIRT_ERR_PROTOCOL;
    uint32_t gate = __atomic_fetch_or(&service->frontend[client].meta->frontend_gate,
        AOS_SERIAL_FRONTEND_CLOSED, __ATOMIC_ACQ_REL);
    if (gate & AOS_SERIAL_FRONTEND_BUSY) return SERIAL_VIRT_ERR_BUSY;
    if (service->frontend_attached[client])
        __atomic_store_n(&service->frontend[client].meta->attached, 0u, __ATOMIC_RELEASE);
    service->guest_attached[client] = 0;
    service->guest[client] = (aos_serial_channel_t){0};
    service->guest_retired[client] = 1;
    return SERIAL_VIRT_OK;
}

uint32_t aos_serial_virt_rebind_validate(const aos_serial_virt_service_t *service,
    uint64_t badge, const serial_virt_rebind_req_t *request, uint32_t length)
{
    if (!service || !request || length != sizeof(*request)) return SERIAL_VIRT_ERR_PROTOCOL;
    if (request->version != SERIAL_VIRT_REBIND_VERSION) return SERIAL_VIRT_ERR_VERSION;
    if (!serial_virt_authorized(badge, request->client, SERIAL_VIRT_ROLE_VMM))
        return SERIAL_VIRT_ERR_AUTHORITY;
    unsigned client = request->client;
    if (!request->generation || service->guest_generation[client] == UINT32_MAX ||
        request->generation != service->guest_generation[client] + 1u)
        return SERIAL_VIRT_ERR_PROTOCOL;
    if (!service->guest_retired[client] || service->guest_attached[client])
        return SERIAL_VIRT_ERR_BUSY;
    if (!service->frontend[client].meta) return SERIAL_VIRT_ERR_PROTOCOL;
    if (__atomic_load_n(&service->frontend[client].meta->frontend_gate,
            __ATOMIC_ACQUIRE) != AOS_SERIAL_FRONTEND_CLOSED) return SERIAL_VIRT_ERR_BUSY;
    return SERIAL_VIRT_OK;
}

static int empty_queue(const aos_serial_queue_handle_t *queue, unsigned capacity)
{
    return queue->queue && queue->data && queue->capacity == capacity &&
        !queue->queue->head && !queue->queue->tail && !queue->queue->producer_signalled;
}

uint32_t aos_serial_virt_rebind_commit(aos_serial_virt_service_t *service,
    uint64_t badge, const serial_virt_rebind_req_t *request, uint32_t length,
    aos_serial_channel_t fresh)
{
    uint32_t status = aos_serial_virt_rebind_validate(service, badge, request, length);
    if (status != SERIAL_VIRT_OK) return status;
    aos_serial_channel_t *front = &service->frontend[request->client];
    if (!fresh.meta || fresh.meta == front->meta || fresh.meta->guest_state ||
        fresh.meta->attached || fresh.meta->frontend_gate ||
        !empty_queue(&fresh.to_guest, AOS_SERIAL_RX_CAPACITY) ||
        !empty_queue(&fresh.from_guest, AOS_SERIAL_TX_CAPACITY) ||
        fresh.to_guest.queue == front->to_guest.queue ||
        fresh.from_guest.queue == front->from_guest.queue ||
        !front->to_guest.queue || !front->to_guest.data ||
        !front->from_guest.queue || !front->from_guest.data ||
        front->to_guest.capacity != AOS_SERIAL_RX_CAPACITY ||
        front->from_guest.capacity != AOS_SERIAL_TX_CAPACITY)
        return SERIAL_VIRT_ERR_PROTOCOL;
    /* The closed gate remains closed throughout reset. Clear bytes as well
     * as indices so neither pending input nor prior guest output survives. */
    for (unsigned i = 0; i < front->to_guest.capacity; i++) front->to_guest.data[i] = 0;
    for (unsigned i = 0; i < front->from_guest.capacity; i++) front->from_guest.data[i] = 0;
    *front->to_guest.queue = (aos_serial_queue_t){0};
    *front->from_guest.queue = (aos_serial_queue_t){0};
    front->meta->guest_state = 0;
    front->meta->attached = 0;
    unsigned client = request->client;
    service->guest[client] = fresh;
    service->guest_attached[client] = 1;
    service->guest_retired[client] = 0;
    service->guest_generation[client] = request->generation;
    __atomic_store_n(&front->meta->frontend_gate, 0u, __ATOMIC_RELEASE);
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
        if (input) result.input_clients |= 1u << i;
        if (output) result.output_clients |= 1u << i;
    }
    return result;
}
