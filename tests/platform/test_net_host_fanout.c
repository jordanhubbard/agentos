#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../services/net-service/net_pd.c"

static uint8_t net_shmem[NETPD_SHMEM_TOTAL];

static int check(int condition, const char *name)
{
    printf("%s - %s\n", condition ? "ok" : "not ok", name);
    return condition ? 0 : 1;
}

static uint32_t open_client(uint32_t iface_id)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};

    req.opcode = MSG_NET_OPEN;
    req.length = 4u;
    data_wr32(req.data, 0, iface_id);
    if (net_pd_dispatch_one(0u, &req, &rep) != SEL4_ERR_OK) {
        return NET_MAX_CLIENTS;
    }
    return data_rd32(rep.data, 4);
}

int main(void)
{
    int failed = 0;
    uint8_t frame[64] = {0};

    memset(net_shmem, 0, sizeof(net_shmem));
    net_pd_shmem_vaddr = (uintptr_t)net_shmem;
    net_pd_test_init();

    uint32_t blocked = open_client(0u);
    uint32_t healthy = open_client(0u);
    if (blocked >= NET_MAX_CLIENTS || healthy >= NET_MAX_CLIENTS) {
        printf("not ok - opened two host fanout clients\n");
        printf("1..1\n");
        return 1;
    }

    for (uint32_t i = 0u; i < 6u; i++) {
        frame[i] = clients[healthy].mac[i];
    }

    volatile netpd_ring_t *blocked_ring =
        slot_ring(clients[blocked].shmem_slot);
    volatile netpd_ring_t *healthy_ring =
        slot_ring(clients[healthy].shmem_slot);
    blocked_ring->rx_head = NETPD_SLOT_DATA_SIZE;
    blocked_ring->rx_tail = 1u;

    failed += check(net_host_deliver(frame, sizeof(frame)),
                    "host descriptor remains recyclable under client backpressure");
    failed += check(blocked_ring->rx_drops == 1u &&
                    clients[blocked].rx_errors == 1u,
                    "backpressured client records an isolated RX drop");
    failed += check(healthy_ring->rx_head == sizeof(frame) + 2u &&
                    healthy_ring->rx_tail == 0u,
                    "healthy client receives the same unicast frame");

    printf("1..3\n");
    return failed == 0 ? 0 : 1;
}
