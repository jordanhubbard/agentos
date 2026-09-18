#include <stdio.h>
#include <platform/blk_rebind.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("not ok - %s\n", #x); } } while (0)
int main(void)
{
    blk_virt_rebind_req_t req = {BLK_VIRT_REBIND_VERSION, 0u, 1u};
    uint64_t badge = virt_client_badge(0u);
    CHECK(sizeof(req) == 12u && sizeof(blk_virt_rebind_reply_t) == 12u);
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
