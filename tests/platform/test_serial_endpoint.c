#include <platform/serial_endpoint.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
static void check(bool result, const char *name)
{
    printf("%s %u - %s\n", result ? "ok" : "not ok", ++checks, name);
    failures += !result;
}
typedef struct {
    uint32_t source, received;
    bool blocked;
    uint8_t input[32];
} device_t;
static uint32_t output(uint8_t *bytes, uint32_t capacity, void *ctx)
{
    device_t *device = ctx;
    uint32_t length = 12u - device->source;
    if (length > 8u) length = 8u;
    if (length > capacity) length = capacity;
    const uint8_t source[] = "ABCDEFGHIJKL";
    memcpy(bytes, &source[device->source], length);
    device->source += length;
    return length;
}
static bool input(const uint8_t *bytes, uint32_t length, void *ctx)
{
    device_t *device = ctx;
    if (device->blocked || length > sizeof(device->input) - device->received) return false;
    memcpy(device->input + device->received, bytes, length);
    device->received += length;
    return true;
}
int main(void)
{
    aos_serial_queue_t rx = {0}, tx = {0};
    uint8_t rx_data[8] = {0}, tx_data[8] = {0}, bytes[16] = {0};
    aos_serial_endpoint_t endpoint = {.channel = {
        .to_guest = {&rx, rx_data, 8}, .from_guest = {&tx, tx_data, 8}}};
    device_t device = {.blocked = true};
    aos_serial_endpoint_ops_t ops = {output, input, &device};
    uint32_t count;
    check(aos_serial_queue_write(&endpoint.channel.to_guest,
              (const uint8_t *)"xyz", 3) == AOS_SERIAL_PUMP_OK, "input accepted into shared queue");
    check(aos_serial_endpoint_step(&endpoint, &ops, false) && rx.head == 0 &&
          tx.tail == 8 && device.received == 0, "paused endpoint exports output without consuming input");
    check(aos_serial_endpoint_step(&endpoint, &ops, true) && rx.head == 3 &&
          endpoint.input_length == 3 && endpoint.output_length == 4 && tx.tail == 8,
          "backpressure retains input and output in private staging");
    check(!aos_serial_endpoint_step(&endpoint, &ops, true) && device.source == 12 &&
          device.received == 0 && endpoint.input_length == 3,
          "repeated blocked retry neither drains more output nor duplicates input");
    check(aos_serial_queue_read(&endpoint.channel.from_guest, bytes, 5, &count) ==
          AOS_SERIAL_PUMP_OK && count == 5 && !memcmp(bytes, "ABCDE", 5),
          "frontend consumes a bounded output prefix");
    device.blocked = false;
    check(aos_serial_endpoint_step(&endpoint, &ops, true) && endpoint.output_length == 0 &&
          endpoint.input_length == 0 && device.received == 3 && !memcmp(device.input, "xyz", 3),
          "downstream recovery delivers retained bytes exactly once");
    check(aos_serial_queue_read(&endpoint.channel.from_guest, bytes, sizeof(bytes), &count) ==
          AOS_SERIAL_PUMP_OK && count == 7 && !memcmp(bytes, "FGHIJKL", 7),
          "ring wrap and staged retry preserve complete output order");
    check(!aos_serial_endpoint_step(&endpoint, &ops, true) && device.received == 3,
          "idle rescan does not redeliver input");

    rx.head = UINT32_MAX - 1u; rx.tail = rx.head;
    check(aos_serial_queue_write(&endpoint.channel.to_guest,
          (const uint8_t *)"0123456", 7) == AOS_SERIAL_PUMP_OK && rx.tail == 5,
          "producer cursor rollover remains bounded");
    uint8_t saved[8]; memcpy(saved, rx_data, sizeof(saved));
    check(aos_serial_queue_write(&endpoint.channel.to_guest,
          (const uint8_t *)"ab", 2) == AOS_SERIAL_PUMP_FULL && rx.tail == 5 &&
          !memcmp(saved, rx_data, sizeof(saved)), "full queue rejects entire input without partial writes");
    check(aos_serial_queue_read(&endpoint.channel.to_guest, bytes, sizeof(bytes), &count) ==
          AOS_SERIAL_PUMP_OK && count == 7 && !memcmp(bytes, "0123456", 7) && rx.head == 5,
          "consumer cursor rollover preserves bytes");
    rx.tail = rx.head + 9;
    memset(bytes, 0x5a, sizeof(bytes));
    check(aos_serial_queue_read(&endpoint.channel.to_guest, bytes, sizeof(bytes), &count) ==
          AOS_SERIAL_PUMP_INVALID && count == 0 && bytes[0] == 0x5a && rx.head == 5,
          "malformed remote length does not read or advance the consumer");
    check(aos_serial_queue_write(&endpoint.channel.to_guest,
          (const uint8_t *)"a", 1) == AOS_SERIAL_PUMP_INVALID && rx.tail == 14,
          "malformed remote length does not advance the producer");
    printf("1..%u\n", checks);
    return failures != 0;
}
