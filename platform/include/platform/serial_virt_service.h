#ifndef AOS_PLATFORM_SERIAL_VIRT_SERVICE_H
#define AOS_PLATFORM_SERIAL_VIRT_SERVICE_H
#include <platform/serial_virt_layout.h>
#include "contracts/serial_virt_contract.h"

typedef struct {
    aos_serial_channel_t guest[AOS_SERIAL_CLIENTS];
    aos_serial_channel_t frontend[AOS_SERIAL_CLIENTS];
    uint8_t guest_attached[AOS_SERIAL_CLIENTS];
    uint8_t frontend_attached[AOS_SERIAL_CLIENTS];
} aos_serial_virt_service_t;

typedef struct {
    uint32_t wake_vmm;       /* bytes arrived or output space became free */
    uint32_t frontend_changed;
    uint32_t invalid_clients;
    uint32_t bytes;
} aos_serial_virt_result_t;

/* Handles are supplied by the trusted PD from the root-owned layout.
 * Zero-initialize the service's attachment arrays once before serving IPC. */
uint32_t aos_serial_virt_attach(aos_serial_virt_service_t *service,
    uint64_t badge, const serial_virt_attach_req_t *request, uint32_t length);
aos_serial_virt_result_t aos_serial_virt_service_pump(
    aos_serial_virt_service_t *service, uint32_t budget_per_direction);
#endif
