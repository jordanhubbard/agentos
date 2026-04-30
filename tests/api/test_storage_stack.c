/*
 * test_storage_stack.c — API tests for block_pd, virtio_blk, and vfs_server
 */

#ifdef AGENTOS_TEST_HOST

#include "framework.h"

void agentos_log_boot(const char *pd_name) { (void)pd_name; }
void agentos_log_channel(const char *pd, uint32_t ch) { (void)pd; (void)ch; }

#define pd_main block_pd_entry
#include "../../kernel/agentos-root-task/src/block_pd.c"
#undef pd_main

#define blk_dma_shmem_vaddr virtio_blk_dma_shmem_vaddr
#define log_drain_rings_vaddr virtio_log_drain_rings_vaddr
#define pd_main virtio_blk_entry
#include "../../kernel/agentos-root-task/src/virtio_blk.c"
#undef pd_main
#undef log_drain_rings_vaddr
#undef blk_dma_shmem_vaddr

#define handles vfs_handles
#define alloc_handle vfs_alloc_handle
#define blk_dma_shmem_vaddr vfs_blk_dma_shmem_vaddr
#define log_drain_rings_vaddr vfs_log_drain_rings_vaddr
#define pd_main vfs_server_entry
#include "../../kernel/agentos-root-task/src/vfs_server.c"
#undef pd_main
#undef log_drain_rings_vaddr
#undef blk_dma_shmem_vaddr
#undef alloc_handle
#undef handles

static void req_set(sel4_msg_t *req, uint32_t off, uint32_t value)
{
    rep_u32(req, off, value);
}

static uint32_t reply_get(const sel4_msg_t *rep, uint32_t off)
{
    return msg_u32(rep, off);
}

static void bpd_dispatch(uint32_t badge, const sel4_msg_t *req, sel4_msg_t *rep)
{
    memset(rep, 0, sizeof(*rep));
    (void)block_pd_h_dispatch((sel4_badge_t)badge, req, rep, (void *)0);
}

static void vblk_dispatch(const sel4_msg_t *req, sel4_msg_t *rep)
{
    memset(rep, 0, sizeof(*rep));
    (void)virtio_blk_h_dispatch(0u, req, rep, (void *)0);
}

static void vfs_dispatch(const sel4_msg_t *req, sel4_msg_t *rep)
{
    memset(rep, 0, sizeof(*rep));
    (void)vfs_server_h_dispatch(0u, req, rep, (void *)0);
}

static void test_block_open_validation(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    block_pd_pd_init();

    req_set(&req, 0, MSG_BLOCK_OPEN);
    req_set(&req, 4, 1u);
    req_set(&req, 8, 0u);
    bpd_dispatch(7u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), BLOCK_ERR_BAD_DEV,
              "block_pd: OPEN rejects unsupported device");
    ASSERT_EQ(reply_get(&rep, 4), 0u,
              "block_pd: bad-device OPEN returns zero handle");

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, MSG_BLOCK_OPEN);
    req_set(&req, 4, 0u);
    req_set(&req, 8, 1u);
    bpd_dispatch(7u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), BLOCK_ERR_BAD_PART,
              "block_pd: OPEN rejects unsupported partition");
    ASSERT_EQ(reply_get(&rep, 4), 0u,
              "block_pd: bad-partition OPEN returns zero handle");
}

static void test_block_handle_lifecycle(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    uint32_t handle;
    block_pd_pd_init();

    req_set(&req, 0, MSG_BLOCK_OPEN);
    req_set(&req, 4, 0u);
    req_set(&req, 8, 0u);
    req_set(&req, 12, BLOCK_OPEN_FLAG_RDONLY);
    bpd_dispatch(7u, &req, &rep);
    handle = reply_get(&rep, 4);
    ASSERT_EQ(reply_get(&rep, 0), BLOCK_OK,
              "block_pd: OPEN whole device succeeds");
    ASSERT_TRUE(handle != 0u, "block_pd: OPEN returns non-zero handle");

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, MSG_BLOCK_STATUS);
    req_set(&req, 4, handle);
    bpd_dispatch(7u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), BLOCK_OK,
              "block_pd: STATUS with owner badge succeeds");
    ASSERT_EQ(reply_get(&rep, 12), 512u,
              "block_pd: STATUS returns sector size");
    ASSERT_TRUE((reply_get(&rep, 16) & BLOCK_STATUS_FLAG_READONLY) != 0u,
                "block_pd: STATUS reports readonly flag");

    bpd_dispatch(8u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), BLOCK_ERR_BAD_HANDLE,
              "block_pd: STATUS rejects non-owner badge");

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, MSG_BLOCK_WRITE);
    req_set(&req, 4, handle);
    req_set(&req, 12, 1u);
    bpd_dispatch(7u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), BLOCK_ERR_READONLY,
              "block_pd: WRITE rejects readonly handle");
    ASSERT_EQ(reply_get(&rep, 4), 0u,
              "block_pd: readonly WRITE reports zero sectors written");

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, MSG_BLOCK_CLOSE);
    req_set(&req, 4, handle);
    bpd_dispatch(7u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), BLOCK_OK, "block_pd: CLOSE succeeds");

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, MSG_BLOCK_STATUS);
    req_set(&req, 4, handle);
    bpd_dispatch(7u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), BLOCK_ERR_BAD_HANDLE,
              "block_pd: STATUS after CLOSE rejects handle");
}

static void test_block_zero_length_and_stubbed_io(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    uint32_t handle;
    block_pd_pd_init();

    req_set(&req, 0, MSG_BLOCK_OPEN);
    req_set(&req, 4, 0u);
    req_set(&req, 8, 0u);
    req_set(&req, 12, 0u);
    bpd_dispatch(3u, &req, &rep);
    handle = reply_get(&rep, 4);

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, MSG_BLOCK_READ);
    req_set(&req, 4, handle);
    req_set(&req, 12, 0u);
    bpd_dispatch(3u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), BLOCK_OK,
              "block_pd: zero-sector READ succeeds");
    ASSERT_EQ(reply_get(&rep, 4), 0u,
              "block_pd: zero-sector READ returns zero actual sectors");

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, MSG_BLOCK_WRITE);
    req_set(&req, 4, handle);
    req_set(&req, 12, 0u);
    bpd_dispatch(3u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), BLOCK_OK,
              "block_pd: zero-sector WRITE succeeds");
    ASSERT_EQ(reply_get(&rep, 4), 0u,
              "block_pd: zero-sector WRITE returns zero actual sectors");

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, MSG_BLOCK_TRIM);
    req_set(&req, 4, handle);
    bpd_dispatch(3u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), BLOCK_OK,
              "block_pd: TRIM is accepted as advisory");

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, MSG_BLOCK_FLUSH);
    req_set(&req, 4, handle);
    bpd_dispatch(3u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), BLOCK_ERR_IO,
              "block_pd: FLUSH fails closed while virtio call is unwired");
}

static void test_virtio_blk_nodev_paths(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};

    blk_mmio_vaddr = 0;
    virtio_blk_dma_shmem_vaddr = 0;
    virtio_blk_pd_init();

    req_set(&req, 0, OP_BLK_INFO);
    vblk_dispatch(&req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), BLK_ERR_NODEV,
              "virtio_blk: INFO returns NODEV before device init");

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, OP_BLK_READ);
    req_set(&req, 12, 0u);
    vblk_dispatch(&req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), BLK_ERR_NODEV,
              "virtio_blk: READ returns NODEV before device init");

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, OP_BLK_HEALTH);
    vblk_dispatch(&req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), BLK_OK,
              "virtio_blk: HEALTH returns OK even when absent");
    ASSERT_EQ(reply_get(&rep, 4), 0u,
              "virtio_blk: HEALTH reports initialized=0");
    ASSERT_EQ(reply_get(&rep, 8), 0u,
              "virtio_blk: HEALTH starts with zero errors");

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, 0xFFFFu);
    vblk_dispatch(&req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), BLK_ERR_IO,
              "virtio_blk: unknown opcode returns IO error");
}

static void test_vfs_front_door(void)
{
    static uint8_t vfs_shmem[VFS_SHMEM_SIZE];
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};

    memset(vfs_shmem, 0, sizeof(vfs_shmem));
    vfs_io_shmem_vaddr = (uintptr_t)vfs_shmem;
    vfs_blk_dma_shmem_vaddr = 0;
    vfs_server_pd_init();

    req_set(&req, 0, OP_VFS_HEALTH);
    vfs_dispatch(&req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), VFS_OK, "vfs_server: HEALTH returns OK");
    ASSERT_EQ(reply_get(&rep, 4), 0u, "vfs_server: HEALTH starts with zero open handles");
    ASSERT_EQ(reply_get(&rep, 8), VFS_VERSION, "vfs_server: HEALTH returns version");

    strcpy((char *)(vfs_io_shmem_vaddr + VFS_SHMEM_PATH_OFF), "/missing");
    memset(&req, 0, sizeof(req));
    req_set(&req, 0, OP_VFS_OPEN);
    req_set(&req, 4, VFS_O_RD);
    vfs_dispatch(&req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), VFS_ERR_NOT_FOUND,
              "vfs_server: OPEN missing path without create returns NOT_FOUND");
    ASSERT_EQ(reply_get(&rep, 4), 0u,
              "vfs_server: failed OPEN returns zero handle");

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, 0xFFFFu);
    vfs_dispatch(&req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), VFS_ERR_INVAL,
              "vfs_server: unknown opcode returns INVAL");
}

int main(void)
{
    TAP_PLAN(32);

    test_block_open_validation();
    test_block_handle_lifecycle();
    test_block_zero_length_and_stubbed_io();
    test_virtio_blk_nodev_paths();
    test_vfs_front_door();

    return tap_exit();
}

#else
int main(void) { return 0; }
#endif
