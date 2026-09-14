#include <libvmm/virtio/console_tx.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    virtio_console_tx_descriptor_t descriptors[4];
    uint8_t bytes[64];
    uint32_t length, room, snapshots;
    bool bad_gpa;
} fixture_t;
static unsigned checks, failures;
static void check(bool ok, const char *name)
{
    printf("%s %u - %s\n", ok ? "ok" : "not ok", ++checks, name);
    failures += !ok;
}
static bool descriptor(void *ctx, uint16_t index, virtio_console_tx_descriptor_t *out)
{
    fixture_t *fixture = ctx;
    if (index >= 4) return false;
    *out = fixture->descriptors[index];
    fixture->snapshots++;
    return true;
}
static uint32_t copy(void *ctx, uint64_t address, uint32_t offset, uint32_t length)
{
    fixture_t *fixture = ctx;
    const uint8_t input[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    if (fixture->bad_gpa || address > sizeof(input) ||
        offset > sizeof(input) - address || length > sizeof(input) - address - offset)
        return UINT32_MAX;
    uint32_t count = length < fixture->room ? length : fixture->room;
    if (count > sizeof(fixture->bytes) - fixture->length) return UINT32_MAX;
    memcpy(fixture->bytes + fixture->length, input + address + offset, count);
    fixture->length += count;
    fixture->room -= count;
    return count;
}
int main(void)
{
    fixture_t fixture = {.descriptors = {{0, 20, 0, 0}}, .room = 8};
    virtio_console_tx_ops_t ops = {descriptor, copy, &fixture};
    virtio_console_tx_state_t state = {0};
    check(virtio_console_tx_step(&state, 0, 4, 8, &ops) == VIRTIO_CONSOLE_TX_WAIT &&
          fixture.length == 8 && state.offset == 8 && state.active,
          "oversized descriptor remains unacknowledged after the first queue fills");
    check(virtio_console_tx_step(&state, 0, 4, 8, &ops) == VIRTIO_CONSOLE_TX_WAIT &&
          fixture.length == 8 && fixture.snapshots == 1,
          "full-queue retry does not recopy bytes or reread descriptor metadata");
    fixture.descriptors[0].length = 1;
    fixture.descriptors[0].address = UINT64_MAX;
    fixture.room = 8;
    check(virtio_console_tx_step(&state, 3, 4, 8, &ops) == VIRTIO_CONSOLE_TX_WAIT &&
          fixture.length == 16 && state.offset == 16,
          "retained descriptor snapshot ignores guest metadata changes while blocked");
    fixture.room = 8;
    check(virtio_console_tx_step(&state, 0, 4, 8, &ops) == VIRTIO_CONSOLE_TX_COMPLETE &&
          !state.active && fixture.length == 20 &&
          !memcmp(fixture.bytes, "abcdefghijklmnopqrst", 20),
          "completion follows exact ordered delivery of the entire oversized descriptor");

    fixture = (fixture_t){.descriptors = {{0, 5, 1, 1}, {5, 7, 0, 0}}, .room = 8};
    state = (virtio_console_tx_state_t){0};
    check(virtio_console_tx_step(&state, 0, 4, 8, &ops) == VIRTIO_CONSOLE_TX_WAIT &&
          state.index == 1 && state.offset == 3 && fixture.length == 8,
          "backpressure retains progress inside the second chained descriptor");
    fixture.room = 8;
    check(virtio_console_tx_step(&state, 0, 4, 8, &ops) == VIRTIO_CONSOLE_TX_COMPLETE &&
          fixture.length == 12 && !memcmp(fixture.bytes, "abcdefghijkl", 12),
          "chain completes without duplicating its prefix");

    fixture = (fixture_t){.descriptors = {{0, 0, 1, 0}}, .room = 8};
    state = (virtio_console_tx_state_t){0};
    check(virtio_console_tx_step(&state, 0, 4, 8, &ops) == VIRTIO_CONSOLE_TX_INVALID &&
          fixture.snapshots == 4 && fixture.length == 0,
          "zero-length cyclic chain is rejected after a bounded number of snapshots");
    check(virtio_console_tx_step(&state, 0, 4, 8, &ops) == VIRTIO_CONSOLE_TX_INVALID &&
          fixture.snapshots == 4, "invalid chain stays failed until device reset");
    fixture = (fixture_t){.descriptors = {{0, 0, 1, 4}}, .room = 8};
    state = (virtio_console_tx_state_t){0};
    check(virtio_console_tx_step(&state, 0, 4, 8, &ops) == VIRTIO_CONSOLE_TX_INVALID &&
          fixture.snapshots == 1, "out-of-range next descriptor is never read");
    state = (virtio_console_tx_state_t){0};
    check(virtio_console_tx_step(&state, 4, 4, 8, &ops) == VIRTIO_CONSOLE_TX_INVALID &&
          fixture.snapshots == 1, "out-of-range available head is never read");
    for (uint16_t flag = 2; flag <= 4; flag += 2) {
        fixture = (fixture_t){.descriptors = {{0, 1, flag, 0}}, .room = 8};
        state = (virtio_console_tx_state_t){0};
        check(virtio_console_tx_step(&state, 0, 4, 8, &ops) == VIRTIO_CONSOLE_TX_INVALID &&
              fixture.length == 0, "writable or unnegotiated indirect TX descriptor is rejected");
    }
    fixture = (fixture_t){.descriptors = {{0, 1, 0, 0}}, .room = 8, .bad_gpa = true};
    state = (virtio_console_tx_state_t){0};
    check(virtio_console_tx_step(&state, 0, 4, 8, &ops) == VIRTIO_CONSOLE_TX_INVALID &&
          fixture.length == 0, "GPA copy failure cannot acknowledge the chain");
    printf("1..%u\n", checks);
    return failures != 0;
}
