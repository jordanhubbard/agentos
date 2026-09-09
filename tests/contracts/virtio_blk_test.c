/*
 * agentOS — virtio_blk PD contract test
 * Covered opcodes: OP_BLK_READ, OP_BLK_WRITE, OP_BLK_FLUSH,
 *   OP_BLK_INFO, OP_BLK_HEALTH
 */
#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/virtio_blk_contract.h"

void run_virtio_blk_tests(microkit_channel ch) {
    TEST_SECTION("virtio_blk");

    /* HEALTH — should always succeed */
    ASSERT_IPC_OK(ch, OP_BLK_HEALTH, "virtio blk health");

    /* INFO — should always succeed */
    ASSERT_IPC_OK(ch, OP_BLK_INFO, "virtio blk info");

    /* READ — out-of-bounds sector */
    seL4_SetMR(1, 0xFFFFFFFF);
    ASSERT_IPC_ERR(ch, OP_BLK_READ, BLK_ERR_OOB, "virtio blk read oob");

    /* WRITE — out-of-bounds sector */
    seL4_SetMR(1, 0xFFFFFFFF);
    ASSERT_IPC_ERR(ch, OP_BLK_WRITE, BLK_ERR_OOB, "virtio blk write oob");

    /* FLUSH — should succeed or error */
    ASSERT_IPC_OK_OR_ERR(ch, OP_BLK_FLUSH, BLK_ERR_IO, "virtio blk flush");

    /* Verify error codes */
    ASSERT_TRUE(BLK_OK == 0, "blk ok == 0");
    ASSERT_TRUE(BLK_ERR_IO == 1, "blk err io == 1");
    ASSERT_TRUE(BLK_ERR_OOB == 2, "blk err oob == 2");
    ASSERT_TRUE(BLK_ERR_NODEV == 3, "blk err nodev == 3");

    /* Version 2 selects canonical media without adding guest-specific ops. */
    ASSERT_TRUE(VIRTIO_BLK_CONTRACT_VERSION == 2u, "virtio blk contract version 2");
    ASSERT_TRUE(BLK_MEDIA_UBUNTU_INSTALL == 0u, "ubuntu media id is stable");
    ASSERT_TRUE(BLK_MEDIA_FREEBSD_INSTALL == 1u, "freebsd media id is stable");
    ASSERT_TRUE(BLK_MEDIA_COUNT == 2u, "two canonical install media endpoints");
    ASSERT_TRUE(sizeof(struct virtio_blk_req_read) == 20u,
                "read request carries media id");
    ASSERT_TRUE(sizeof(struct virtio_blk_req_info) == 8u,
                "info request carries media id");
}
