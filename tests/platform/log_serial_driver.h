/* Host bridge to the production serial_pd dispatcher, in a separate TU. */
#pragma once
#include <stdint.h>

void log_serial_driver_init(uint8_t *shared);
uint32_t log_serial_driver_call(uint32_t opcode, uint32_t length,
                               const uint8_t data[48],
                               uint32_t *reply_length, uint8_t reply_data[48]);
void log_serial_capture(char c);
