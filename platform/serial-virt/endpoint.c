#include <platform/serial_endpoint.h>

bool aos_serial_endpoint_step(aos_serial_endpoint_t *endpoint,
    const aos_serial_endpoint_ops_t *ops, bool running)
{
    if (!endpoint || !ops || !ops->input || !ops->output ||
        endpoint->input_length > AOS_SERIAL_ENDPOINT_CHUNK ||
        endpoint->output_length > AOS_SERIAL_ENDPOINT_CHUNK) return false;
    bool changed = false;
    if (!endpoint->output_length) {
        endpoint->output_length = ops->output(endpoint->output,
            AOS_SERIAL_ENDPOINT_CHUNK, ops->context);
        if (endpoint->output_length > AOS_SERIAL_ENDPOINT_CHUNK) return false;
    }
    if (endpoint->output_length && aos_serial_queue_write(
            &endpoint->channel.from_guest, endpoint->output,
            endpoint->output_length) == AOS_SERIAL_PUMP_OK) {
        endpoint->output_length = 0;
        changed = true;
    }
    if (running) {
        if (!endpoint->input_length) {
            if (aos_serial_queue_read(&endpoint->channel.to_guest, endpoint->input,
                    AOS_SERIAL_ENDPOINT_CHUNK, &endpoint->input_length) == AOS_SERIAL_PUMP_OK &&
                endpoint->input_length) changed = true;
        }
        if (endpoint->input_length && ops->input(endpoint->input,
                endpoint->input_length, ops->context)) endpoint->input_length = 0;
    }
    return changed;
}
