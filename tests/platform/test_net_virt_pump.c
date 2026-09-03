/*
 * Host test for aos_net_virt_pump. No seL4. No sDDF headers.
 *
 * gcc -I platform/include tests/platform/test_net_virt_pump.c \
 *     platform/net-virt/net_virt_pump.c -o test_net_virt_pump
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <platform/net_layout.h>
#include <platform/net_virt_pump.h>

#define PASS(name) do { printf("  PASS  %s\n", name); return 0; } while (0)
#define FAIL(msg)  do { printf("  FAIL  %s:%d: %s\n", __FILE__, __LINE__, msg); return 1; } while (0)
#define CHECK(cond) do { if (!(cond)) FAIL(#cond); } while (0)

static uint8_t g_region[AOS_NET_CLIENT_STRIDE * 2u];

static int test_abi_sizes(void)
{
    CHECK(sizeof(aos_net_buff_desc_t) == 16u);
    CHECK(AOS_NET_BUFFER_SIZE == 2048u);
    CHECK(AOS_NET_CAPACITY == 32u);
    CHECK(AOS_VIRTIO_NET_GUEST_IPA == 0x0A010000UL);
    CHECK(AOS_VIRTIO_NET_GUEST_IPA != 0x0A000000UL);
    CHECK(AOS_VIRTIO_NET_VIRQ == 50u);
    PASS("test_abi_sizes");
}

static int test_empty_pump(void)
{
    aos_net_virt_t v;
    aos_net_virt_client_t c;

    memset(g_region, 0, sizeof(g_region));
    aos_net_virt_reset(&v);
    aos_net_client_bind(g_region, 0u, &c);
    aos_net_client_init_buffers(&c);
    CHECK(aos_net_virt_add_client(&v, &c) == 0);
    CHECK(aos_net_virt_pump(&v) == 0u);
    PASS("test_empty_pump");
}

static int enqueue_tx(aos_net_virt_client_t *c, const uint8_t *frame, uint16_t len)
{
    aos_net_buff_desc_t buf;
    uint32_t cap = c->capacity;

    if (c->tx_free->tail == c->tx_free->head) {
        return -1;
    }
    buf = c->tx_free->buffers[c->tx_free->head % cap];
    c->tx_free->head++;
    memcpy(c->tx_data + (uint32_t)buf.io_or_offset, frame, len);
    buf.len = len;
    c->tx_active->buffers[c->tx_active->tail % cap] = buf;
    c->tx_active->tail++;
    return 0;
}

static int test_loopback(void)
{
    aos_net_virt_t v;
    aos_net_virt_client_t c;
    uint8_t frame[64];
    uint32_t i;
    aos_net_buff_desc_t rx;
    uint32_t cap;

    memset(g_region, 0, sizeof(g_region));
    memset(frame, 0x5a, sizeof(frame));
    frame[0] = 0xff;
    frame[1] = 0xff;
    frame[5] = 0xff;
    for (i = 12; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)i;
    }

    aos_net_virt_reset(&v);
    aos_net_client_bind(g_region, 0u, &c);
    aos_net_client_init_buffers(&c);
    CHECK(aos_net_virt_add_client(&v, &c) == 0);

    CHECK(enqueue_tx(&c, frame, (uint16_t)sizeof(frame)) == 0);
    CHECK(aos_net_virt_pump(&v) == 1u);

    cap = c.capacity;
    CHECK((uint16_t)(c.rx_active->tail - c.rx_active->head) == 1u);
    rx = c.rx_active->buffers[c.rx_active->head % cap];
    CHECK(rx.len == (uint16_t)sizeof(frame));
    CHECK(memcmp(c.rx_data + (uint32_t)rx.io_or_offset, frame, sizeof(frame)) == 0);
    CHECK((uint16_t)(c.tx_free->tail - c.tx_free->head) == (uint16_t)AOS_NET_CAPACITY);
    PASS("test_loopback");
}

static int test_hub_two_clients(void)
{
    aos_net_virt_t v;
    aos_net_virt_client_t a;
    aos_net_virt_client_t b;
    uint8_t frame[32];
    aos_net_buff_desc_t rx;
    uint32_t cap;

    memset(g_region, 0, sizeof(g_region));
    memset(frame, 0x11, sizeof(frame));
    frame[0] = 0x02;

    aos_net_virt_reset(&v);
    aos_net_client_bind(g_region, 0u, &a);
    aos_net_client_bind(g_region, 1u, &b);
    aos_net_client_init_buffers(&a);
    aos_net_client_init_buffers(&b);
    CHECK(aos_net_virt_add_client(&v, &a) == 0);
    CHECK(aos_net_virt_add_client(&v, &b) == 0);

    CHECK(enqueue_tx(&a, frame, (uint16_t)sizeof(frame)) == 0);
    CHECK(aos_net_virt_pump(&v) == 1u);

    CHECK((uint16_t)(a.rx_active->tail - a.rx_active->head) == 0u);
    cap = b.capacity;
    CHECK((uint16_t)(b.rx_active->tail - b.rx_active->head) == 1u);
    rx = b.rx_active->buffers[b.rx_active->head % cap];
    CHECK(rx.len == (uint16_t)sizeof(frame));
    CHECK(memcmp(b.rx_data + (uint32_t)rx.io_or_offset, frame, sizeof(frame)) == 0);
    PASS("test_hub_two_clients");
}

static int test_drop_when_rx_full(void)
{
    aos_net_virt_t v;
    aos_net_virt_client_t c;
    uint8_t frame[16];
    uint16_t free_before;

    memset(g_region, 0, sizeof(g_region));
    memset(frame, 0xaa, sizeof(frame));
    aos_net_virt_reset(&v);
    aos_net_client_bind(g_region, 0u, &c);
    aos_net_client_init_buffers(&c);
    CHECK(aos_net_virt_add_client(&v, &c) == 0);

    /* Steal every RX free buffer so the pump must drop. */
    c.rx_free->head = c.rx_free->tail;
    CHECK(enqueue_tx(&c, frame, (uint16_t)sizeof(frame)) == 0);
    free_before = (uint16_t)(c.tx_free->tail - c.tx_free->head);
    CHECK(aos_net_virt_pump(&v) == 0u);
    CHECK((uint16_t)(c.rx_active->tail - c.rx_active->head) == 0u);
    CHECK((uint16_t)(c.tx_free->tail - c.tx_free->head) == (uint16_t)(free_before + 1u));
    PASS("test_drop_when_rx_full");
}

int main(void)
{
    int failed = 0;

    printf("net_virt_pump\n");
    failed += test_abi_sizes();
    failed += test_empty_pump();
    failed += test_loopback();
    failed += test_hub_two_clients();
    failed += test_drop_when_rx_full();
    if (failed) {
        printf("%d test(s) failed\n", failed);
        return 1;
    }
    printf("All net_virt_pump tests passed\n");
    return 0;
}
