#include <stdint.h>
#include <stdio.h>

#include <platform/net_layout.h>
#include <platform/net_rx_drain.h>

typedef struct {
    uint32_t remaining;
    uint32_t received;
    uint32_t receive_calls;
    uint32_t flush_calls;
    uint32_t stop_after_first_batch;
} drain_fixture_t;

static uint32_t receive_batch(void *opaque, uint32_t limit)
{
    drain_fixture_t *fixture = opaque;
    uint32_t count;

    fixture->receive_calls++;
    if (fixture->stop_after_first_batch && fixture->receive_calls > 1u) {
        return 0u;
    }
    count = fixture->remaining < limit ? fixture->remaining : limit;
    fixture->remaining -= count;
    fixture->received += count;
    return count;
}

static void flush_batch(void *opaque)
{
    drain_fixture_t *fixture = opaque;
    fixture->flush_calls++;
}

static int check(int condition, const char *name)
{
    printf("%s - %s\n", condition ? "ok" : "not ok", name);
    return condition ? 0 : 1;
}

int main(void)
{
    int failed = 0;
    drain_fixture_t sustained = { .remaining = 80u };
    drain_fixture_t bounded = { .remaining = 200u };
    drain_fixture_t backpressured = {
        .remaining = 80u,
        .stop_after_first_batch = 1u,
    };
    uint32_t count;

    count = aos_net_rx_drain(receive_batch, flush_batch, &sustained,
                             AOS_NET_CAPACITY);
    failed += check(count == 80u && sustained.remaining == 0u &&
                    sustained.receive_calls == 3u &&
                    sustained.flush_calls == 6u &&
                    count > AOS_NET_CAPACITY && count * 256u > 15u * 1024u,
                    "coalesced RX drains beyond 32 descriptors and 15 KiB");

    count = aos_net_rx_drain(receive_batch, flush_batch, &bounded,
                             AOS_NET_CAPACITY);
    failed += check(count == AOS_NET_CAPACITY * AOS_NET_RX_DRAIN_BATCHES &&
                    bounded.remaining == 72u && bounded.receive_calls == 4u,
                    "RX work remains bounded under a continuous stream");

    count = aos_net_rx_drain(receive_batch, flush_batch, &backpressured,
                             AOS_NET_CAPACITY);
    failed += check(count == AOS_NET_CAPACITY &&
                    backpressured.remaining == 48u &&
                    backpressured.receive_calls == 2u,
                    "guest backpressure stops without consuming another batch");

    printf("1..3\n");
    return failed == 0 ? 0 : 1;
}
