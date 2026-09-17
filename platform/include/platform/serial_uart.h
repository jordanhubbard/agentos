#ifndef AOS_SERIAL_UART_H
#define AOS_SERIAL_UART_H
#include <platform/serial_virt_layout.h>
#include <stdbool.h>

/* x86 firmware composition: serial_pd alone receives this eight-port cap.
 * COM1 remains bootstrap diagnostics. COM2 is the serial queue frontend. */
#define AOS_SERIAL_UART_PORT 0x2f8u
#define AOS_SERIAL_UART_CAP_SLOT 35u
typedef struct {
    bool (*read)(void *context, unsigned offset, uint8_t *value);
    bool (*write)(void *context, unsigned offset, uint8_t value);
    void *context;
} aos_serial_uart_io_t;
typedef struct {
    aos_serial_channel_t channel;
    uint8_t tx, rx;
    bool tx_pending, rx_pending;
} aos_serial_uart_t;

/* 16550, 38400 baud, 8N1, FIFO, polling (hardware interrupts disabled). */
bool aos_serial_uart_init(const aos_serial_uart_io_t *io);
/* One byte per direction at most. Retains bytes across queue/UART pressure.
 * Returns false on invalid queue or hardware I/O failure. changed reports
 * shared cursor movement and must cause a serial_virt notification. */
bool aos_serial_uart_step(aos_serial_uart_t *, const aos_serial_uart_io_t *, bool *changed);
#endif
