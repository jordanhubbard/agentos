#ifndef AOS_X86_CONFIG_H
#define AOS_X86_CONFIG_H
#include <stdbool.h>
#include <stdint.h>
#include "platform/x86_rtc.h"

typedef struct {
    uint32_t pci_address, ram_bytes, fw_offset;
    uint16_t fw_selector;
    uint8_t host[256], pm[256], cmos_index;
    uint32_t pci_reads, timer_reads, fw_reads;
    uint32_t cpu_selector;
    uint8_t cpu_command;
    aos_x86_rtc_t rtc;
} aos_x86_config_t;

/* One state per guest; timer_ticks is supplied by the VMM's virtual clock.
 * False rejects unsupported ports/widths without mutating state or value. */
bool aos_x86_config_init(aos_x86_config_t *s, uint32_t ram_bytes);
bool aos_x86_config_io(aos_x86_config_t *s, uint16_t port, unsigned width,
                       bool write, uint32_t *value, uint64_t timer_ticks);
#endif
