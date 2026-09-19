#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <platform/cc_serial_control.h>

static bool receive(cc_serial_control_t *s, uint16_t event, uint16_t value)
{
    uint8_t packet[8] = {0, 0, 0, 0, event, event >> 8, value, value >> 8};
    return cc_serial_control_receive(s, packet, sizeof(packet));
}

int main(void)
{
    cc_serial_control_t s = {0};
    assert(!receive(&s, CC_SERIAL_PORT_OPEN, 1));
    assert(receive(&s, CC_SERIAL_PORT_ADD, 1));
    assert(s.present && s.ready_pending && s.open_pending && !s.host_open);
    assert(receive(&s, CC_SERIAL_PORT_OPEN, 1) && s.host_open);
    assert(receive(&s, CC_SERIAL_PORT_OPEN, 0));
    assert(!s.host_open && s.close_pending);
    assert(receive(&s, CC_SERIAL_PORT_OPEN, 1));
    assert(s.host_open && s.close_pending); /* Close/open in one RX batch. */
    cc_serial_control_reset(&s);
    assert(!s.present && !s.host_open && !s.ready_pending && !s.open_pending);
    assert(s.close_pending);
    assert(receive(&s, CC_SERIAL_PORT_ADD, 1));
    assert(s.close_pending && s.ready_pending && s.open_pending);
    s.ready_pending = s.open_pending = false;
    assert(receive(&s, CC_SERIAL_PORT_ADD, 1));
    assert(!s.ready_pending && !s.open_pending); /* Idempotent add. */
    assert(receive(&s, CC_SERIAL_PORT_REMOVE, 1));
    assert(!s.present && !s.host_open && s.close_pending);
    cc_serial_control_t before = s;
    const uint16_t invalid_events[] = {CC_SERIAL_DEVICE_READY, CC_SERIAL_PORT_READY,
        CC_SERIAL_CONSOLE_PORT, CC_SERIAL_RESIZE, UINT16_MAX};
    for (unsigned i = 0; i < sizeof(invalid_events)/sizeof(*invalid_events); ++i) {
        assert(!receive(&s, invalid_events[i], 1));
        assert(!memcmp(&s, &before, sizeof(s)));
    }
    uint8_t packet[8] = {1, 0, 0, 0, CC_SERIAL_PORT_ADD, 0, 1, 0};
    assert(!cc_serial_control_receive(&s, packet, sizeof(packet)));
    packet[0] = 0;
    for (size_t n = 0; n < 8u; ++n)
        assert(!cc_serial_control_receive(&s, packet, n));
    assert(!memcmp(&s, &before, sizeof(s)));
    assert(!cc_serial_control_receive(NULL, packet, sizeof(packet)));
    assert(!cc_serial_control_receive(&s, NULL, sizeof(packet)));
    assert(!receive(&s, CC_SERIAL_PORT_ADD, 0));
    assert(!receive(&s, CC_SERIAL_PORT_REMOVE, 0));
    assert(!memcmp(&s, &before, sizeof(s)));
    assert(receive(&s, CC_SERIAL_PORT_ADD, 1));
    before = s;
    assert(!receive(&s, CC_SERIAL_PORT_OPEN, 2));
    assert(!memcmp(&s, &before, sizeof(s)));
    const uint8_t name[] = {0,0,0,0,CC_SERIAL_PORT_NAME,0,1,0,'c','c','.','0',0};
    assert(cc_serial_control_receive(&s, name, sizeof(name)));
    assert(!memcmp(&s, &before, sizeof(s)));
    s = (cc_serial_control_t){0};
    cc_serial_control_reset(&s);
    assert(!s.close_pending);
    assert(receive(&s, CC_SERIAL_PORT_ADD, 1));
    assert(receive(&s, CC_SERIAL_PORT_OPEN, 1));
    assert(!s.close_pending);
    cc_serial_control_reset(&s);
    assert(s.close_pending && !s.host_open && !s.present);
    const uint16_t outbound[] = {CC_SERIAL_DEVICE_READY, CC_SERIAL_PORT_READY,
                                 CC_SERIAL_PORT_OPEN};
    for (unsigned i = 0; i < sizeof(outbound)/sizeof(*outbound); ++i) {
        memset(packet, 0xff, sizeof(packet));
        assert(cc_serial_control_encode(outbound[i], packet));
        const uint8_t expected[8] = {0,0,0,0,outbound[i],0,1,0};
        assert(!memcmp(packet, expected, sizeof(packet)));
    }
    puts("PASS: CC control handshake, close retention, reset and rejected events");
}
