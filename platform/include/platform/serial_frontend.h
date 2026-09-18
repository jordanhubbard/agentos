#ifndef AOS_PLATFORM_SERIAL_FRONTEND_H
#define AOS_PLATFORM_SERIAL_FRONTEND_H
#include <stdbool.h>
#include <platform/serial_virt_layout.h>

/* The service can close admission concurrently, but may retire/reset queues
 * only after BUSY clears. Never reopen this gate without a generation/reset
 * protocol; ending an operation preserves the service's CLOSED bit. */
static inline aos_serial_pump_status_t aos_serial_frontend_begin(aos_serial_channel_t *channel)
{
    if (!channel || !channel->meta) return AOS_SERIAL_PUMP_INVALID;
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&channel->meta->frontend_gate, &expected,
            AOS_SERIAL_FRONTEND_BUSY, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return AOS_SERIAL_PUMP_OK;
    return expected & AOS_SERIAL_FRONTEND_CLOSED ? AOS_SERIAL_PUMP_INVALID : AOS_SERIAL_PUMP_FULL;
}

static inline void aos_serial_frontend_end(aos_serial_channel_t *channel)
{
    __atomic_fetch_and(&channel->meta->frontend_gate, ~AOS_SERIAL_FRONTEND_BUSY, __ATOMIC_RELEASE);
}

static inline aos_serial_pump_status_t aos_serial_frontend_write(
    aos_serial_channel_t *channel, const uint8_t *bytes, uint32_t length)
{
    aos_serial_pump_status_t result = aos_serial_frontend_begin(channel);
    if (result != AOS_SERIAL_PUMP_OK) return result;
    result = aos_serial_queue_write(&channel->to_guest, bytes, length);
    aos_serial_frontend_end(channel);
    return result;
}

static inline aos_serial_pump_status_t aos_serial_frontend_read(
    aos_serial_channel_t *channel, uint8_t *bytes, uint32_t capacity, uint32_t *length)
{
    if (!length) return AOS_SERIAL_PUMP_INVALID;
    *length = 0;
    aos_serial_pump_status_t result = aos_serial_frontend_begin(channel);
    if (result != AOS_SERIAL_PUMP_OK) return result;
    result = aos_serial_queue_read(&channel->from_guest, bytes, capacity, length);
    aos_serial_frontend_end(channel);
    return result;
}
#endif
