#include <platform/serial_uart.h>

bool aos_serial_uart_init(const aos_serial_uart_io_t *io)
{
    if (!io || !io->read || !io->write) return false;
    const uint8_t offsets[]={1,3,0,1,3,2,4};
    const uint8_t values[]={0,0x80,3,0,3,0xc7,0x0b};
    for (unsigned i=0;i<sizeof(offsets);i++)
        if (!io->write(io->context,offsets[i],values[i])) return false;
    return true;
}

bool aos_serial_uart_step(aos_serial_uart_t *uart,
                          const aos_serial_uart_io_t *io, bool *changed)
{
    if (!uart || !io || !io->read || !io->write || !changed) return false;
    *changed=false;
    if (!uart->tx_pending) {
        uint32_t n=0;
        if (aos_serial_queue_read(&uart->channel.from_guest,&uart->tx,1,&n)
            !=AOS_SERIAL_PUMP_OK) return false;
        uart->tx_pending=n!=0;
        *changed=uart->tx_pending;
    }
    uint8_t status;
    if (!io->read(io->context,5,&status) || (status & 0x1eu)) return false;
    if (uart->tx_pending && (status & 0x20u)) {
        if (!io->write(io->context,0,uart->tx)) return false;
        uart->tx_pending=false;
    }
    if (!uart->rx_pending && (status & 1u)) {
        if (!io->read(io->context,0,&uart->rx)) return false;
        uart->rx_pending=true;
    }
    if (uart->rx_pending) {
        aos_serial_pump_status_t result=aos_serial_queue_write(
            &uart->channel.to_guest,&uart->rx,1);
        if (result==AOS_SERIAL_PUMP_INVALID) return false;
        if (result==AOS_SERIAL_PUMP_OK) {
            uart->rx_pending=false;
            *changed=true;
        }
    }
    return true;
}
