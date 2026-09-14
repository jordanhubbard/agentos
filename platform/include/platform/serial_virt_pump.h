#ifndef AOS_PLATFORM_SERIAL_VIRT_PUMP_H
#define AOS_PLATFORM_SERIAL_VIRT_PUMP_H

#include <stdint.h>

/* sDDF serial_queue_t wire layout. Queue handles and capacities stay private
 * to the PD; untrusted peers supply only shared indices and byte storage. */
typedef struct {
    uint32_t tail;
    uint32_t head;
    uint32_t producer_signalled;
} aos_serial_queue_t;

typedef struct {
    aos_serial_queue_t *queue;
    uint8_t *data;
    uint32_t capacity;
} aos_serial_queue_handle_t;

typedef enum {
    AOS_SERIAL_PUMP_OK = 0,
    AOS_SERIAL_PUMP_INVALID = 1,
} aos_serial_pump_status_t;

/* One bounded transfer. The caller is the source consumer and destination
 * producer; each queue has exactly one of each. The queues and data regions
 * must be disjoint, with capacities fixed by the root-owned layout.
 * Backpressure leaves unread bytes in the source. Malformed indices leave
 * both queues untouched. Returns transferred bytes separately from status.
 * The caller performs sDDF notification/recheck handling after the transfer. */
aos_serial_pump_status_t aos_serial_virt_transfer(
    const aos_serial_queue_handle_t *source,
    const aos_serial_queue_handle_t *destination,
    uint32_t budget, uint32_t *transferred);

#endif
