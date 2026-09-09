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

    uint32_t freebsd = open_client(1u);
    if (freebsd >= NET_MAX_CLIENTS) {
        printf("not ok - opened routed FreeBSD client\n");
        printf("1..4\n");
        return 1;
    }
    volatile netpd_ring_t *freebsd_ring =
        slot_ring(clients[freebsd].shmem_slot);
    uint32_t healthy_head = healthy_ring->rx_head;
    for (uint32_t i = 0u; i < 6u; i++) {
        frame[i] = iface_mac[i];
    }
    frame[12] = 0x08u;
    frame[13] = 0x00u;
    frame[14] = 0x45u;
    frame[30] = 10u;
    frame[31] = 0u;
    frame[32] = 2u;
    frame[33] = 16u;

    failed += check(net_host_deliver(frame, sizeof(frame)) &&
                    freebsd_ring->rx_head == sizeof(frame) + 2u &&
                    healthy_ring->rx_head == healthy_head,
                    "QEMU host-forwarded IPv4 reaches only its assigned client");
    const uint8_t *freebsd_frame =
        net_shmem + NETPD_SLOT_OFFSET(clients[freebsd].shmem_slot) +
        NETPD_SLOT_HDR_SIZE + 2u;
    failed += check(memcmp(freebsd_frame, clients[freebsd].mac, 6u) == 0,
                    "QEMU IPv4 demux rewrites the selected virtual MAC");

    uint8_t arp_reply[42] = {0};
    memcpy(arp_reply, iface_mac, 6u);
    arp_reply[12] = 0x08u;
    arp_reply[13] = 0x06u;
    arp_reply[14] = 0x00u;
    arp_reply[15] = 0x01u;
    arp_reply[16] = 0x08u;
    arp_reply[17] = 0x00u;
    arp_reply[18] = 6u;
    arp_reply[19] = 4u;
    arp_reply[20] = 0x00u;
    arp_reply[21] = 0x02u;
    memcpy(arp_reply + 32u, iface_mac, 6u);
    arp_reply[38] = 10u;
    arp_reply[39] = 0u;
    arp_reply[40] = 2u;
    arp_reply[41] = 16u;
    uint32_t freebsd_before_arp = freebsd_ring->rx_head;
    uint32_t healthy_before_arp = healthy_ring->rx_head;
    failed += check(net_host_deliver(arp_reply, sizeof(arp_reply)) &&
                    freebsd_ring->rx_head ==
                        freebsd_before_arp + sizeof(arp_reply) + 2u &&
                    healthy_ring->rx_head == healthy_before_arp,
                    "QEMU ARP reply reaches only its target-IP client");
    const uint8_t *freebsd_arp =
        net_shmem + NETPD_SLOT_OFFSET(clients[freebsd].shmem_slot) +
        NETPD_SLOT_HDR_SIZE + freebsd_before_arp + 2u;
    failed += check(memcmp(freebsd_arp, clients[freebsd].mac, 6u) == 0 &&
                    memcmp(freebsd_arp + 32u, clients[freebsd].mac, 6u) == 0,
                    "QEMU ARP demux rewrites Ethernet and target identities");

    uint8_t arp[42] = {0};
    for (uint32_t i = 0u; i < 6u; i++) {
        arp[6u + i] = clients[freebsd].mac[i];
        arp[22u + i] = clients[freebsd].mac[i];
    }
    arp[12] = 0x08u;
    arp[13] = 0x06u;
    arp[14] = 0x00u;
    arp[15] = 0x01u;
    arp[16] = 0x08u;
    arp[17] = 0x00u;
    arp[18] = 6u;
    arp[19] = 4u;
    net_host_translate_tx(arp, sizeof(arp));
    failed += check(memcmp(arp + 6u, iface_mac, 6u) == 0 &&
                    memcmp(arp + 22u, iface_mac, 6u) == 0,
                    "QEMU egress rewrites Ethernet and ARP source identities");

    printf("1..8\n");
    return failed == 0 ? 0 : 1;
}
