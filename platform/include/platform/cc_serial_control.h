#ifndef AOS_CC_SERIAL_CONTROL_H
#define AOS_CC_SERIAL_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* VirtIO console control messages are little-endian, independent of CC's
 * data-frame ABI. The deployment must explicitly bind CC to port one.
 * Queues 2/3 carry control RX/TX; port one uses data queues 4/5. */
#define CC_SERIAL_PORT_ID 1u
enum {
    CC_SERIAL_DEVICE_READY = 0,
    CC_SERIAL_PORT_ADD = 1,
    CC_SERIAL_PORT_REMOVE = 2,
    CC_SERIAL_PORT_READY = 3,
    CC_SERIAL_CONSOLE_PORT = 4,
    CC_SERIAL_RESIZE = 5,
    CC_SERIAL_PORT_OPEN = 6,
    CC_SERIAL_PORT_NAME = 7,
};
typedef struct {
    bool present;
    bool host_open;
    bool ready_pending;
    bool open_pending;
    /* A reconnect cannot clear this latch. Only the coordinator may clear
     * it after invalidating old frames/cache and retaining input releases. */
    bool close_pending;
} cc_serial_control_t;

bool cc_serial_control_receive(cc_serial_control_t *state,
                               const uint8_t *packet, size_t length);
/* Encode a guest-origin control message; fixed CC port one. */
bool cc_serial_control_encode(uint16_t event, uint8_t packet[8]);
/* Transport reset loses connection identity. Retain any close obligation
 * through reset; the initialization path must repeat DEVICE_READY. */
void cc_serial_control_reset(cc_serial_control_t *state);

#endif
