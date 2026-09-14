#include <libvmm/virtio/console_tx_ring.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
static void check(bool condition, const char *name)
{
    printf("%s %u - %s\n", condition ? "ok" : "not ok", ++checks, name);
    failures += !condition;
}
typedef struct { uint32_t room, length; uint8_t bytes[64]; } output_t;
static uint32_t copy(void *ctx, uint64_t address, uint32_t offset, uint32_t length)
{
    output_t *output = ctx;
    const uint8_t source[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    if (address > sizeof(source) || offset > sizeof(source) - address ||
        length > sizeof(source) - address - offset) return UINT32_MAX;
    uint32_t count = length < output->room ? length : output->room;
    if (count > sizeof(output->bytes) - output->length) return UINT32_MAX;
    memcpy(output->bytes + output->length, source + address + offset, count);
    output->length += count; output->room -= count;
    return count;
}
int main(void)
{
    struct virtq_desc descriptors[4] = {{.addr = 0, .len = 20}};
    struct virtq ring = {.num = 4, .desc = descriptors};
    ring.avail = calloc(1, sizeof(*ring.avail) + 4 * sizeof(uint16_t));
    ring.used = calloc(1, sizeof(*ring.used) + 4 * sizeof(struct virtq_used_elem));
    if (!ring.avail || !ring.used) return 1;
    virtio_console_tx_state_t state = {0};
    uint16_t last = 0, head = 0;
    output_t output = {.room = 8};
    ring.avail->idx = 1;
    virtio_console_tx_ring_result_t result = virtio_console_tx_ring_run(
        &ring, 4, &last, &head, &state, 8, copy, &output);
    check(result.valid && result.bytes == 8 && !result.completed &&
          !ring.used->idx && !last, "partial descriptor produces no used entry or available-index advance");
    result = virtio_console_tx_ring_run(&ring, 4, &last, &head, &state, 8, copy, &output);
    check(result.valid && !result.bytes && !result.completed && !ring.used->idx && !last,
          "full backend retry cannot acknowledge or duplicate partial output");
    ring.avail->ring[0] = 3; /* Guest mutation must not replace retained head. */
    output.room = 8;
    result = virtio_console_tx_ring_run(&ring, 4, &last, &head, &state, 8, copy, &output);
    check(result.valid && result.bytes == 8 && !result.completed && !ring.used->idx,
          "retained available head survives mutation during backpressure");
    output.room = 8;
    result = virtio_console_tx_ring_run(&ring, 4, &last, &head, &state, 8, copy, &output);
    check(result.valid && result.bytes == 4 && result.completed == 1 && last == 1 &&
          ring.used->idx == 1 && ring.used->ring[0].id == 0 && ring.used->ring[0].len == 0 &&
          output.length == 20 && !memcmp(output.bytes, "abcdefghijklmnopqrst", 20),
          "exact complete stream publishes the original used head once");
    result = virtio_console_tx_ring_run(&ring, 4, &last, &head, &state, 8, copy, &output);
    check(result.valid && !result.bytes && !result.completed && ring.used->idx == 1,
          "idle rescan does not republish completed descriptors");

    state = (virtio_console_tx_state_t){0}; output = (output_t){.room = 64};
    last = UINT16_MAX; ring.used->idx = UINT16_MAX; ring.avail->idx = 0;
    ring.avail->ring[3] = 1; descriptors[1] = (struct virtq_desc){.addr = 20, .len = 1};
    result = virtio_console_tx_ring_run(&ring, 4, &last, &head, &state, 8, copy, &output);
    check(result.valid && result.completed == 1 && last == 0 && ring.used->idx == 0 &&
          ring.used->ring[3].id == 1 && output.bytes[0] == 'u',
          "available and used cursor rollover publishes the correct ring entry");
    state = (virtio_console_tx_state_t){0}; output = (output_t){.room = 64};
    ring.avail->idx = 5;
    result = virtio_console_tx_ring_run(&ring, 4, &last, &head, &state, 8, copy, &output);
    check(!result.valid && !result.completed && !result.bytes && !last && !ring.used->idx,
          "overrun available index fails without reading descriptors or publishing used entries");

    state = (virtio_console_tx_state_t){0}; output = (output_t){.room = 64};
    ring.avail->idx = 3;
    for (uint16_t i = 0; i < 3; i++) {
        ring.avail->ring[i] = i;
        descriptors[i] = (struct virtq_desc){.addr = 4u * i, .len = 4};
    }
    result = virtio_console_tx_ring_run(&ring, 4, &last, &head, &state, 6, copy, &output);
    check(result.valid && result.bytes == 6 && result.completed == 1 && last == 1 &&
          ring.used->idx == 1, "one byte budget bounds copying across multiple available heads");
    result = virtio_console_tx_ring_run(&ring, 4, &last, &head, &state, 8, copy, &output);
    check(result.valid && result.bytes == 6 && result.completed == 2 && last == 3 &&
          ring.used->idx == 3 && ring.used->ring[1].id == 1 && ring.used->ring[2].id == 2 &&
          !memcmp(output.bytes, "abcdefghijkl", 12), "retry completes remaining heads in order");

    state = (virtio_console_tx_state_t){0}; output = (output_t){.room = 8};
    last = 0; ring.used->idx = 0; ring.avail->idx = 1;
    descriptors[0] = (struct virtq_desc){.addr = 0, .len = 2, .flags = 1, .next = 4};
    result = virtio_console_tx_ring_run(&ring, 4, &last, &head, &state, 8, copy, &output);
    check(!result.valid && !result.completed && !last && !ring.used->idx,
          "invalid chained descriptor never publishes completion");
    state = (virtio_console_tx_state_t){0};
    descriptors[0] = (struct virtq_desc){.addr = 0, .len = 20};
    output = (output_t){.room = 1};
    (void)virtio_console_tx_ring_run(&ring, 4, &last, &head, &state, 8, copy, &output);
    ring.avail->idx = 0;
    result = virtio_console_tx_ring_run(&ring, 4, &last, &head, &state, 8, copy, &output);
    check(!result.valid && !result.completed && !ring.used->idx,
          "guest cannot retract the available entry while its chain is pending");
    free(ring.avail); free(ring.used);
    printf("1..%u\n", checks);
    return failures != 0;
}
