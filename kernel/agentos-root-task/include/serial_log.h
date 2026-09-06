/*
 * serial_log.h — bounded line-buffered logging through serial_pd
 *
 * Non-driver PDs must never map the platform UART.  They write into the
 * shared serial transfer page and invoke the existing MSG_SERIAL_* contract.
 */

#pragma once

#include <stdint.h>
#include "sel4_client.h"
#include "system_desc.h"
#include "contracts/serial_contract.h"

#define AGENTOS_SERIAL_SHMEM_VA 0x10005000UL

typedef struct {
    seL4_CPtr ep;
    uint32_t slot;
    uint32_t used;
    uint8_t opened;
    uint8_t buffer[SERIAL_MAX_WRITE_BYTES];
} serial_log_t;

static inline uint32_t serial_log_rd32(const uint8_t *data, uint32_t off)
{
    return (uint32_t)data[off]
         | ((uint32_t)data[off + 1u] << 8)
         | ((uint32_t)data[off + 2u] << 16)
         | ((uint32_t)data[off + 3u] << 24);
}

static inline void serial_log_wr32(uint8_t *data, uint32_t off, uint32_t value)
{
    data[off] = (uint8_t)value;
    data[off + 1u] = (uint8_t)(value >> 8);
    data[off + 2u] = (uint8_t)(value >> 16);
    data[off + 3u] = (uint8_t)(value >> 24);
}

static inline int serial_log_open(serial_log_t *log)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};

    if (log->opened) {
        return 1;
    }
    if (log->ep == seL4_CapNull) {
        return 0;
    }

    req.opcode = MSG_SERIAL_OPEN;
    req.length = sizeof(struct serial_req_open);
    serial_log_wr32(req.data, 0u, 0u);
    sel4_call(log->ep, &req, &rep);
    if (serial_log_rd32(rep.data, 0u) != SERIAL_OK) {
        return 0;
    }

    log->slot = serial_log_rd32(rep.data, 4u);
    log->opened = 1u;
    return 1;
}

static inline void serial_log_flush(serial_log_t *log)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    volatile uint8_t *shared = (volatile uint8_t *)AGENTOS_SERIAL_SHMEM_VA;

    if (log->used == 0u || !serial_log_open(log)) {
        log->used = 0u;
        return;
    }

    for (uint32_t i = 0u; i < log->used; i++) {
        shared[i] = log->buffer[i];
    }

    req.opcode = MSG_SERIAL_WRITE;
    req.length = sizeof(struct serial_req_write);
    serial_log_wr32(req.data, 0u, log->slot);
    serial_log_wr32(req.data, 4u, log->used);
    sel4_call(log->ep, &req, &rep);
    log->used = 0u;
}

static inline void serial_log_putc(serial_log_t *log, char character)
{
    log->buffer[log->used++] = (uint8_t)character;
    if (character == '\n' || log->used == SERIAL_MAX_WRITE_BYTES) {
        serial_log_flush(log);
    }
}

static inline void serial_log_puts(serial_log_t *log, const char *text)
{
    for (; text && *text; text++) {
        serial_log_putc(log, *text);
    }
    serial_log_flush(log);
}
