#include <stdio.h>
#include <platform/net_rebind.h>
#include <platform/net_virt_pump.h>
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("not ok - %s\n", #x); } } while (0)
int main(void)
{
    net_virt_rebind_req_t req = {NET_VIRT_REBIND_VERSION, 0u, 1u};
    for (uint32_t client = 0; client < AOS_NET_GUEST_CLIENTS; client++) {
        req.client = client;
        uint64_t badge = virt_client_badge(client);
        aos_net_virt_client_t bound = {0};
        aos_net_client_bind((uint8_t *)AOS_NET_SHMEM_VA, client, &bound);
        uintptr_t va = aos_net_rebind_queue_va(client);
        CHECK((uintptr_t)bound.rx_free == va + AOS_NET_RX_FREE_OFF);
        CHECK((uintptr_t)bound.tx_active == va + AOS_NET_TX_ACTIVE_OFF);
        CHECK((uintptr_t)bound.tx_data + AOS_NET_TX_DATA_BYTES <= va + AOS_NET_SHMEM_FRAME_SIZE);
        CHECK(aos_net_rebind_validate(badge, &req, sizeof(req), false, true, 0u) == NET_VIRT_OK);
        CHECK(aos_net_rebind_validate(badge, &req, sizeof(req), true, true, 0u) == NET_VIRT_ERR_BUSY);
        CHECK(aos_net_rebind_validate(badge, &req, sizeof(req), false, false, 0u) == NET_VIRT_ERR_BUSY);
        CHECK(aos_net_rebind_validate(virt_client_badge(client ^ 1u), &req, sizeof(req), false, true, 0u) == NET_VIRT_ERR_BAD_CLIENT);
        CHECK(aos_net_rebind_validate(badge, &req, sizeof(req) - 1u, false, true, 0u) == NET_VIRT_ERR_VERSION);
        CHECK(aos_net_rebind_validate(badge, &req, sizeof(req), false, true, 1u) == NET_VIRT_ERR_BUSY);
    }
    req.client = AOS_NET_NATIVE_CLIENT;
    CHECK(aos_net_rebind_queue_va(req.client) == 0u);
    CHECK(aos_net_rebind_validate(VIRT_NET_BADGE_NATIVE, &req, sizeof(req), false, true, 0u) == NET_VIRT_ERR_BAD_CLIENT);
    req.client = 0u;
    uint64_t badge = virt_client_badge(0u);
    req.generation = 0u;
    CHECK(aos_net_rebind_validate(badge, &req, sizeof(req), false, true, 0u) == NET_VIRT_ERR_BUSY);
    req.generation = 2u;
    CHECK(aos_net_rebind_validate(badge, &req, sizeof(req), false, true, 0u) == NET_VIRT_ERR_BUSY);
    CHECK(aos_net_rebind_validate(badge, &req, sizeof(req), false, true, 1u) == NET_VIRT_OK);
    req.generation = UINT32_MAX;
    CHECK(aos_net_rebind_validate(badge, &req, sizeof(req), false, true, UINT32_MAX - 1u) == NET_VIRT_OK);
    CHECK(aos_net_rebind_validate(badge, &req, sizeof(req), false, true, UINT32_MAX) == NET_VIRT_ERR_BUSY);
    req.version++;
    CHECK(aos_net_rebind_validate(badge, &req, sizeof(req), false, true, 0u) == NET_VIRT_ERR_VERSION);
    CHECK(aos_net_rebind_validate(badge, NULL, 0u, false, true, 0u) == NET_VIRT_ERR_VERSION);
    net_virt_rebind_reply_t reply = {
        .status = NET_VIRT_OK, .version = NET_VIRT_REBIND_VERSION,
        .generation = 3u, .hw_state = NET_VIRT_HW_NET_PD,
        .mac = {0x52, 0x54, 0, 0x12, 0x34, 0x57}
    };
    CHECK(sizeof(reply) == 24u);
    CHECK(aos_net_rebind_reply_valid(&reply, sizeof(reply), 3u));
    CHECK(!aos_net_rebind_reply_valid(NULL, sizeof(reply), 3u));
    CHECK(!aos_net_rebind_reply_valid(&reply, 12u, 3u));
    CHECK(!aos_net_rebind_reply_valid(&reply, sizeof(reply) + 1u, 3u));
    CHECK(!aos_net_rebind_reply_valid(&reply, sizeof(reply), 2u));
    CHECK(!aos_net_rebind_reply_valid(&reply, sizeof(reply), 0u));
    reply.status = NET_VIRT_ERR_RESOURCE;
    CHECK(!aos_net_rebind_reply_valid(&reply, sizeof(reply), 3u));
    reply.status = NET_VIRT_OK;
    reply.version++;
    CHECK(!aos_net_rebind_reply_valid(&reply, sizeof(reply), 3u));
    reply.version = NET_VIRT_REBIND_VERSION;
    reply.hw_state = UINT32_MAX;
    CHECK(!aos_net_rebind_reply_valid(&reply, sizeof(reply), 3u));
    reply.hw_state = NET_VIRT_HW_NONE;
    CHECK(aos_net_rebind_reply_valid(&reply, sizeof(reply), 3u));
    reply._pad[0] = 1u;
    CHECK(!aos_net_rebind_reply_valid(&reply, sizeof(reply), 3u));
    reply._pad[0] = 0u;
    reply._pad[1] = 1u;
    CHECK(!aos_net_rebind_reply_valid(&reply, sizeof(reply), 3u));
    printf("%u network rebind checks, %u failures\n", checks, failures);
    return failures != 0u;
}
