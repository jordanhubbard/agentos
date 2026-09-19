#include <platform/cc_serial_control.h>

bool cc_serial_control_receive(cc_serial_control_t *s,
                               const uint8_t *p, size_t length)
{
    if (!s || !p || length < 8u) return false;
    uint32_t port = (uint32_t)p[0] | (uint32_t)p[1] << 8 |
                    (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    uint16_t event = (uint16_t)p[4] | (uint16_t)p[5] << 8;
    uint16_t value = (uint16_t)p[6] | (uint16_t)p[7] << 8;
    if (port != 0u) return false;
    switch (event) {
    case CC_SERIAL_PORT_ADD:
        if (length != 8u || value != 1u) return false;
        if (!s->present) {
            s->present = true;
            s->ready_pending = true;
            s->open_pending = true;
        }
        return true;
    case CC_SERIAL_PORT_REMOVE:
        if (length != 8u || value != 1u) return false;
        s->close_pending |= s->present || s->host_open;
        s->present = s->host_open = false;
        s->ready_pending = s->open_pending = false;
        return true;
    case CC_SERIAL_PORT_OPEN:
        if (length != 8u || value > 1u || !s->present) return false;
        /* Even an initially closed port must invalidate stale transport
         * state before admitting the next connection. */
        if (!value) s->close_pending = true;
        s->host_open = value != 0u;
        return true;
    case CC_SERIAL_PORT_NAME:
        return s->present && value == 1u && length > 8u;
    case CC_SERIAL_CONSOLE_PORT:
        /* CC requires a serial port with host connection notifications. */
        return false;
    default:
        return false;
    }
}

bool cc_serial_control_encode(uint16_t event, uint8_t packet[8])
{
    if (!packet || (event != CC_SERIAL_DEVICE_READY &&
        event != CC_SERIAL_PORT_READY && event != CC_SERIAL_PORT_OPEN)) return false;
    for (unsigned i = 0; i < 8u; ++i) packet[i] = 0u;
    packet[4] = (uint8_t)event;
    packet[5] = (uint8_t)(event >> 8);
    packet[6] = 1u;
    return true;
}

void cc_serial_control_reset(cc_serial_control_t *s)
{
    if (!s) return;
    bool close = s->close_pending || s->present || s->host_open;
    *s = (cc_serial_control_t){.close_pending = close};
}
