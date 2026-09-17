/* Canonical serial_pd's x86 queue frontend. No guest RAM, host MMIO or
 * other I/O ports are accessible through this driver's capability. */
#include <sel4/sel4.h>
#include "serial_virt_client.h"
#include "system_desc.h"
#include <platform/serial_uart.h>
#include "agentos.h"

uintptr_t log_drain_rings_vaddr;

static bool uart_read(void *context, unsigned offset, uint8_t *value)
{
    (void)context;
    if (offset>=8 || !value) return false;
    seL4_X86_IOPort_In8_t result=seL4_X86_IOPort_In8(
        AOS_SERIAL_UART_CAP_SLOT,AOS_SERIAL_UART_PORT+offset);
    if (result.error) return false;
    *value=(uint8_t)result.result;
    return true;
}
static bool uart_write(void *context, unsigned offset, uint8_t value)
{
    (void)context;
    return offset<8 && !seL4_X86_IOPort_Out8(AOS_SERIAL_UART_CAP_SLOT,
                                            AOS_SERIAL_UART_PORT+offset,value);
}
void pd_main(seL4_CPtr endpoint, seL4_CPtr nameserver)
{
    (void)endpoint; (void)nameserver;
    const aos_serial_uart_io_t io={.read=uart_read,.write=uart_write};
    aos_serial_uart_t uart={.channel=aos_serial_channel_at(
        AOS_SERIAL_SHMEM_VA+AOS_SERIAL_FRONTEND_FRAME*AOS_SERIAL_FRAME_SIZE)};
    if (!aos_serial_uart_init(&io) ||
        !serial_virt_client_attach(0,SERIAL_VIRT_ROLE_FRONTEND)) goto failed;
    for (;;) {
        bool changed;
        if (!aos_serial_uart_step(&uart,&io,&changed)) goto failed;
        if (changed) seL4_Signal(PD_CNODE_SLOT_SERIAL_VIRT_NOTIFY);
        seL4_Yield();
    }
failed:
    agentos_log_info("serial_pd","UART frontend FAILED");
    for (;;) seL4_Yield();
}
