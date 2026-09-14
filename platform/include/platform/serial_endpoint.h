#ifndef AOS_SERIAL_ENDPOINT_H
#define AOS_SERIAL_ENDPOINT_H
#include <stdbool.h>
#include <platform/serial_virt_layout.h>

#define AOS_SERIAL_ENDPOINT_CHUNK 256u
typedef struct {
    aos_serial_channel_t channel;
    uint8_t input[AOS_SERIAL_ENDPOINT_CHUNK], output[AOS_SERIAL_ENDPOINT_CHUNK];
    uint32_t input_length, output_length;
} aos_serial_endpoint_t;
typedef struct {
    /* Output returns at most capacity. Input accepts all bytes or none. */
    uint32_t (*output)(uint8_t *, uint32_t, void *);
    bool (*input)(const uint8_t *, uint32_t, void *);
    void *context;
} aos_serial_endpoint_ops_t;

/* One bounded chunk each way. Input stays queued while the guest is paused.
 * Failed downstream acceptance retains the already-consumed input locally.
 * The caller signals the virtualizer whenever shared cursors changed. */
bool aos_serial_endpoint_step(aos_serial_endpoint_t *,
    const aos_serial_endpoint_ops_t *, bool running);
#endif
