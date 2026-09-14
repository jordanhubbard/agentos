#include <platform/serial_virt_pump.h>
#include <stdio.h>
#include <string.h>

static int failures;
static unsigned checks;
static void check(int result, const char *name)
{
    printf("%s %u - %s\n", result ? "ok" : "not ok", ++checks, name);
    failures += !result;
}

int main(void)
{
    aos_serial_queue_t sq = {0}, dq = {0}, other = {0};
    uint8_t source[8] = "abcdefg", destination[8] = {0}, foreign[8] = "private";
    aos_serial_queue_handle_t src = {&sq, source, 8};
    aos_serial_queue_handle_t dst = {&dq, destination, 8};
    uint32_t count;
    sq.tail = 7;
    check(aos_serial_virt_transfer(&src, &dst, 3, &count) == AOS_SERIAL_PUMP_OK &&
          count == 3 && sq.head == 3 && dq.tail == 3 &&
          !memcmp(destination, "abc", 3), "budget bounds the transferred prefix");
    check(aos_serial_virt_transfer(&src, &dst, 8, &count) == AOS_SERIAL_PUMP_OK &&
          count == 4 && !memcmp(destination, "abcdefg", 7), "remaining bytes retain order");
    source[7] = 'h'; source[0] = 'i'; sq.tail = 9;
    check(aos_serial_virt_transfer(&src, &dst, 8, &count) == AOS_SERIAL_PUMP_OK &&
          count == 1 && sq.head == 8 && dq.tail == 8 && destination[7] == 'h',
          "destination capacity applies backpressure without losing source bytes");
    check(aos_serial_virt_transfer(&src, &dst, 8, &count) == AOS_SERIAL_PUMP_OK &&
          count == 0 && sq.head == 8 && dq.tail == 8, "full destination preserves cursors");
    dq.head = 1;
    check(aos_serial_virt_transfer(&src, &dst, 8, &count) == AOS_SERIAL_PUMP_OK &&
          count == 1 && sq.head == 9 && destination[0] == 'i', "consumer progress permits retry");

    sq.head = UINT32_MAX - 1u; sq.tail = 1;
    dq.head = UINT32_MAX; dq.tail = UINT32_MAX;
    source[6] = 'x'; source[7] = 'y'; source[0] = 'z';
    check(aos_serial_virt_transfer(&src, &dst, 8, &count) == AOS_SERIAL_PUMP_OK &&
          count == 3 && sq.head == 1 && dq.tail == 2 &&
          destination[7] == 'x' && destination[0] == 'y' && destination[1] == 'z',
          "unsigned cursor rollover preserves bytes and queue lengths");

    uint8_t before[8]; memcpy(before, destination, 8);
    sq.tail = sq.head + 9;
    uint32_t old_head = sq.head, old_tail = dq.tail;
    check(aos_serial_virt_transfer(&src, &dst, 8, &count) == AOS_SERIAL_PUMP_INVALID &&
          count == 0 && sq.head == old_head && dq.tail == old_tail &&
          !memcmp(before, destination, 8), "malformed source length fails without writes");
    sq.tail = sq.head + 1; dq.head = dq.tail - 9;
    check(aos_serial_virt_transfer(&src, &dst, 8, &count) == AOS_SERIAL_PUMP_INVALID &&
          count == 0 && sq.head == old_head && dq.tail == old_tail &&
          !memcmp(before, destination, 8), "malformed destination length fails without writes");
    dq.head = dq.tail; dst.capacity = 7;
    check(aos_serial_virt_transfer(&src, &dst, 8, &count) == AOS_SERIAL_PUMP_INVALID,
          "non-power-of-two capacity is rejected");
    dst.capacity = 8;
    check(aos_serial_virt_transfer(&src, &src, 8, &count) == AOS_SERIAL_PUMP_INVALID,
          "self-transfer is rejected");
    aos_serial_queue_t other_dst = {0};
    uint8_t other_output[8] = {0};
    aos_serial_queue_handle_t other_source = {&other, foreign, 8};
    aos_serial_queue_handle_t other_destination = {&other_dst, other_output, 8};
    other.tail = 7;
    check(aos_serial_virt_transfer(&other_source, &other_destination, 8, &count) ==
          AOS_SERIAL_PUMP_OK && count == 7 && !memcmp(other_output, "private", 7) &&
          !memcmp(before, destination, 8) && sq.head == old_head && dq.tail == old_tail,
          "a second client transfers its own bytes without changing the first");
    printf("1..%u\n", checks);
    return failures ? 1 : 0;
}
