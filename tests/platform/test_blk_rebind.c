#include <stdio.h>
#include <platform/blk_rebind.h>
#include <platform/blk_virt_pump.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("not ok - %s\n", #x); } } while (0)
int main(void)
{
    blk_virt_rebind_req_t req = {BLK_VIRT_REBIND_VERSION, 0u, 1u};
    uint64_t badge = virt_client_badge(0u);
    for (uint32_t client = 0; client < AOS_BLK_MAX_CLIENTS; client++) {
        aos_blk_virt_client_t bound = {0};
        aos_blk_client_bind((uint8_t *)AOS_BLK_SHMEM_VA, client, &bound);
        uintptr_t va = aos_blk_rebind_queue_va(client);
        CHECK(va >= AOS_BLK_SHMEM_VA + AOS_BLK_SHMEM_FRAME_SIZE);
        CHECK((uintptr_t)bound.info == va + AOS_BLK_STORAGE_INFO_OFF);
        CHECK((uintptr_t)bound.req == va + AOS_BLK_REQ_QUEUE_OFF);
        CHECK((uintptr_t)bound.resp == va + AOS_BLK_RESP_QUEUE_OFF);
        CHECK((uintptr_t)bound.data + AOS_BLK_DATA_BYTES <= va + AOS_BLK_SHMEM_FRAME_SIZE);
    }
    CHECK(aos_blk_rebind_queue_va(AOS_BLK_MAX_CLIENTS) == 0u);
    CHECK(sizeof(req) == 12u && sizeof(blk_virt_rebind_reply_t) == 16u);
    blk_virt_rebind_reply_t reply = {BLK_VIRT_OK, BLK_VIRT_REBIND_VERSION, 1u, BLK_VIRT_HW_NONE};
    CHECK(aos_blk_rebind_reply_valid(&reply, sizeof(reply), 1u));
    reply.hw_state = BLK_VIRT_HW_VIRTIO_BLK;
    CHECK(aos_blk_rebind_reply_valid(&reply, sizeof(reply), 1u));
    CHECK(!aos_blk_rebind_reply_valid(NULL, sizeof(reply), 1u));
    CHECK(!aos_blk_rebind_reply_valid(&reply, sizeof(reply) - 1u, 1u));
    CHECK(!aos_blk_rebind_reply_valid(&reply, sizeof(reply) + 1u, 1u));
    CHECK(!aos_blk_rebind_reply_valid(&reply, sizeof(reply), 0u));
    CHECK(!aos_blk_rebind_reply_valid(&reply, sizeof(reply), 2u));
    reply.hw_state = UINT32_MAX;
    CHECK(!aos_blk_rebind_reply_valid(&reply, sizeof(reply), 1u));
    reply.hw_state = BLK_VIRT_HW_NONE;
    reply.status = BLK_VIRT_ERR_BUSY;
    CHECK(!aos_blk_rebind_reply_valid(&reply, sizeof(reply), 1u));
    reply.status = BLK_VIRT_OK;
    reply.version = 1u;
    CHECK(!aos_blk_rebind_reply_valid(&reply, sizeof(reply), 1u));
    CHECK(aos_blk_rebind_validate(badge, &req, sizeof(req), false, true, 0u) == BLK_VIRT_OK);
    CHECK(aos_blk_rebind_validate(badge, NULL, sizeof(req), false, true, 0u) == BLK_VIRT_ERR_VERSION);
    CHECK(aos_blk_rebind_validate(badge, &req, sizeof(req) - 1u, false, true, 0u) == BLK_VIRT_ERR_VERSION);
    CHECK(aos_blk_rebind_validate(badge, &req, sizeof(req) + 1u, false, true, 0u) == BLK_VIRT_ERR_VERSION);
    CHECK(aos_blk_rebind_validate(badge, &req, sizeof(req), true, true, 0u) == BLK_VIRT_ERR_BUSY);
    CHECK(aos_blk_rebind_validate(badge, &req, sizeof(req), false, false, 0u) == BLK_VIRT_ERR_BUSY);
    CHECK(aos_blk_rebind_validate(virt_client_badge(1u), &req, sizeof(req), false, true, 0u) == BLK_VIRT_ERR_BAD_CLIENT);
    CHECK(aos_blk_rebind_validate(VIRT_NET_BADGE_NATIVE, &req, sizeof(req), false, true, 0u) == BLK_VIRT_ERR_BAD_CLIENT);
    req.version++;
    CHECK(aos_blk_rebind_validate(badge, &req, sizeof(req), false, true, 0u) == BLK_VIRT_ERR_VERSION);
    req.version = BLK_VIRT_REBIND_VERSION;
    req.client = UINT32_MAX;
    CHECK(aos_blk_rebind_validate(badge, &req, sizeof(req), false, true, 0u) == BLK_VIRT_ERR_BAD_CLIENT);
    req.client = 1u;
    badge = virt_client_badge(1u);
    CHECK(aos_blk_rebind_validate(badge, &req, sizeof(req), false, true, 0u) == BLK_VIRT_OK);
    req.generation = 0u;
    CHECK(aos_blk_rebind_validate(badge, &req, sizeof(req), false, true, 0u) == BLK_VIRT_ERR_BUSY);
    req.generation = 2u;
    CHECK(aos_blk_rebind_validate(badge, &req, sizeof(req), false, true, 0u) == BLK_VIRT_ERR_BUSY);
    CHECK(aos_blk_rebind_validate(badge, &req, sizeof(req), false, true, 1u) == BLK_VIRT_OK);
    CHECK(aos_blk_rebind_validate(badge, &req, sizeof(req), false, true, 2u) == BLK_VIRT_ERR_BUSY);
    req.generation = UINT32_MAX;
    CHECK(aos_blk_rebind_validate(badge, &req, sizeof(req), false, true, UINT32_MAX - 1u) == BLK_VIRT_OK);
    CHECK(aos_blk_rebind_validate(badge, &req, sizeof(req), false, true, UINT32_MAX) == BLK_VIRT_ERR_BUSY);
    printf("%u block rebind checks, %u failures\n", checks, failures);
    return failures != 0u;
}
