#include "log_serial_driver.h"

#define serial_shmem_vaddr driver_serial_shmem_vaddr
#define SERIAL_PD_TEST_PUTC log_serial_capture
#include "../../services/serial-mux/serial_pd.c"

void log_serial_driver_init(uint8_t *shared)
{
    serial_pd_test_init();
    serial_shmem_vaddr = (uintptr_t)shared;
    /* Only the UART byte sink is substituted; dispatch and TX are real. */
    hw_ready = true;
}

uint32_t log_serial_driver_call(uint32_t opcode, uint32_t length,
                               const uint8_t data[48],
                               uint32_t *reply_length, uint8_t reply_data[48])
{
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = opcode;
    req.length = length;
    memcpy(req.data, data, sizeof(req.data));
    uint32_t rc = serial_pd_dispatch_one(0, &req, &rep);
    *reply_length = rep.length;
    memcpy(reply_data, rep.data, sizeof(rep.data));
    return rc;
}
