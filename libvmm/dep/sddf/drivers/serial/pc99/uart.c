/*
 * Copyright 2025, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdbool.h>
#include <stdint.h>
#include <sel4/sel4.h>
#include <sddf/util/printf.h>
#include <sddf/util/util.h>
#include <sddf/resources/device.h>
#include <sddf/serial/config.h>
#include <sddf/serial/queue.h>

__attribute__((__section__(".device_resources"))) device_resources_t device_resources;
__attribute__((__section__(".serial_driver_config"))) serial_driver_config_t config;

// @billn Need a way to express io port in sdfgen config structure
#define IOPORT_ID 0
#define IOPORT_BASE 0x3f8
// @billn need some sort of "machine description" format for x86 sdfgen to automatically pull in IRQ
#define IRQ_ID 1

/*
 * Port offsets
 * W    - write
 * R    - read
 * RW   - read and write
 * DLAB - Alternate register function bit
 */

#define SERIAL_THR  0 /* Transmitter Holding Buffer (W ) DLAB = 0 */
#define SERIAL_RBR  0 /* Receiver Buffer            (R ) DLAB = 0 */
#define SERIAL_DLL  0 /* Divisor Latch Low Byte     (RW) DLAB = 1 */
#define SERIAL_IER  1 /* Interrupt Enable Register  (RW) DLAB = 0 */
#define SERIAL_DLH  1 /* Divisor Latch High Byte    (RW) DLAB = 1 */
#define SERIAL_IIR  2 /* Interrupt Identification   (R ) */
#define SERIAL_FCR  2 /* FIFO Control Register      (W ) */
#define SERIAL_LCR  3 /* Line Control Register      (RW) */
#define SERIAL_MCR  4 /* Modem Control Register     (RW) */
#define SERIAL_LSR  5 /* Line Status Register       (R ) */
#define SERIAL_MSR  6 /* Modem Status Register      (R ) */
#define SERIAL_SR   7 /* Scratch Register           (RW) */
#define SERIAL_DLAB BIT(7)
#define SERIAL_LSR_DATA_READY BIT(0)
#define SERIAL_LSR_TRANSMITTER_EMPTY BIT(5)

enum irq_state { MODEM_STATUS = 0, TX_HOLD_REG_EMPTY, RX_DATA_AVAIL, RX_LINE_STS };

serial_queue_handle_t rx_queue_handle;
serial_queue_handle_t tx_queue_handle;

static seL4_CPtr g_ioport_cap;

void write(uint16_t port_offset, uint8_t v)
{
    seL4_X86_IOPort_Out8(g_ioport_cap, IOPORT_BASE + port_offset, v);
}

uint8_t read(uint16_t port_offset)
{
    seL4_X86_IOPort_In8_t r = seL4_X86_IOPort_In8(g_ioport_cap, IOPORT_BASE + port_offset);
    return (uint8_t)r.result;
}

int tx_ready(void)
{
    return read(SERIAL_LSR) & SERIAL_LSR_TRANSMITTER_EMPTY;
}

int rx_ready(void)
{
    return read(SERIAL_LSR) & SERIAL_LSR_DATA_READY;
}

static seL4_CPtr g_ep;
static seL4_CPtr g_irq_cap;

static void tx_provide(void)
{
    bool transferred = false;
    char c;
    while (!serial_queue_empty(&tx_queue_handle, tx_queue_handle.queue->head)) {
        serial_dequeue(&tx_queue_handle, &c);
        while (!tx_ready());
        write(SERIAL_THR, c);
        transferred = true;
    }

    if (transferred && serial_require_consumer_signal(&tx_queue_handle)) {
        serial_cancel_consumer_signal(&tx_queue_handle);
        seL4_Signal(config.tx.id);
    }
}

static void rx_return(void)
{
    bool enqueued = false;
    while (rx_ready() && !serial_queue_full(&rx_queue_handle, rx_queue_handle.queue->tail)) {
        char c = (char)read(SERIAL_RBR);
        serial_enqueue(&rx_queue_handle, c);
        enqueued = true;
    }

    if (enqueued) {
        seL4_Signal(config.rx.id);
    }
}

static void handle_irq(void)
{
    uint8_t iir = read(SERIAL_IIR) >> 1;
    if (iir & RX_DATA_AVAIL) {
        rx_return();
    }
}

static void pd_notified(seL4_Word badge)
{
    seL4_Word ch = badge;
    if (ch == config.tx.id) {
        tx_provide();
    } else if (ch == config.rx.id) {
        rx_return();
    } else if (ch == IRQ_ID) {
        handle_irq();
        seL4_IRQHandler_Ack(g_irq_cap);
    } else {
        sddf_dprintf("UART|LOG: received notification on unexpected channel: %lu\n", ch);
    }
}

void serial_drv_main(seL4_CPtr ep, seL4_CPtr ioport_cap, seL4_CPtr irq_cap)
{
    g_ep = ep;
    g_ioport_cap = ioport_cap;
    g_irq_cap = irq_cap;

    assert(serial_config_check_magic(&config));
    assert(device_resources_check_magic(&device_resources));

    if (config.rx_enabled) {
        serial_queue_init(&rx_queue_handle, config.rx.queue.vaddr, config.rx.data.size, config.rx.data.vaddr);
    }
    serial_queue_init(&tx_queue_handle, config.tx.queue.vaddr, config.tx.data.size, config.tx.data.vaddr);

    while (!(read(SERIAL_LSR) & 0x60));

    write(SERIAL_LCR, 0x00);
    if (config.rx_enabled) {
        write(SERIAL_IER, 0x01);
    } else {
        write(SERIAL_IER, 0x00);
    }
    write(SERIAL_LCR, 0x80);
    write(SERIAL_DLL, 0x01);
    write(SERIAL_DLH, 0x00);
    write(SERIAL_LCR, 0x03);
    write(SERIAL_MCR, 0x0b);
    write(SERIAL_FCR, 0x00);

    read(SERIAL_RBR);
    read(SERIAL_LSR);
    read(SERIAL_MSR);

    seL4_Word badge;
    while (1) {
        seL4_MessageInfo_t info = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(info);
        if (label == seL4_Fault_NullFault) {
            pd_notified(badge);
        } else {
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
        }
    }
}
