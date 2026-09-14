#include <platform/serial_virt_pump.h>

static int valid_handle(const aos_serial_queue_handle_t *handle)
{
    return handle && handle->queue && handle->data && handle->capacity &&
           !(handle->capacity & (handle->capacity - 1u));
}

aos_serial_pump_status_t aos_serial_virt_transfer(
    const aos_serial_queue_handle_t *source,
    const aos_serial_queue_handle_t *destination,
    uint32_t budget, uint32_t *transferred)
{
    if (!transferred) return AOS_SERIAL_PUMP_INVALID;
    *transferred = 0u;
    if (!valid_handle(source) || !valid_handle(destination) ||
        source->queue == destination->queue || source->data == destination->data)
        return AOS_SERIAL_PUMP_INVALID;

    /* Snapshot remote indices once. A malicious peer cannot turn a bounded
     * transfer into an unbounded rescan or enlarge either data region. */
    uint32_t source_head = __atomic_load_n(&source->queue->head, __ATOMIC_RELAXED);
    uint32_t source_tail = __atomic_load_n(&source->queue->tail, __ATOMIC_ACQUIRE);
    uint32_t destination_tail = __atomic_load_n(&destination->queue->tail, __ATOMIC_RELAXED);
    uint32_t destination_head = __atomic_load_n(&destination->queue->head, __ATOMIC_ACQUIRE);
    uint32_t available = source_tail - source_head;
    uint32_t occupied = destination_tail - destination_head;
    if (available > source->capacity || occupied > destination->capacity)
        return AOS_SERIAL_PUMP_INVALID;

    uint32_t count = destination->capacity - occupied;
    if (count > available) count = available;
    if (count > budget) count = budget;
    for (uint32_t i = 0u; i < count; i++) {
        destination->data[(destination_tail + i) & (destination->capacity - 1u)] =
            source->data[(source_head + i) & (source->capacity - 1u)];
    }
    if (count) {
        __atomic_store_n(&destination->queue->tail, destination_tail + count, __ATOMIC_RELEASE);
        __atomic_store_n(&source->queue->head, source_head + count, __ATOMIC_RELEASE);
    }
    *transferred = count;
    return AOS_SERIAL_PUMP_OK;
}
