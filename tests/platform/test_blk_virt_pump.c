/*
 * Host test for aos_blk_virt_pump. No seL4. No sDDF headers.
 *
 * gcc -I platform/include tests/platform/test_blk_virt_pump.c \
 *     platform/blk-virt/blk_virt_pump.c -o test_blk_virt_pump
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>

#include <platform/blk_layout.h>
#include <platform/blk_virt_pump.h>
#include <platform/net_layout.h>

#define PASS(name) do { printf("  PASS  %s\n", name); return 0; } while (0)
#define FAIL(msg)  do { printf("  FAIL  %s:%d: %s\n", __FILE__, __LINE__, msg); return 1; } while (0)
#define CHECK(cond) do { if (!(cond)) FAIL(#cond); } while (0)

static uint8_t g_region[AOS_BLK_SHMEM_SIZE];

static int setup_one(aos_blk_virt_t *v, aos_blk_virt_client_t *c)
{
    memset(g_region, 0, sizeof(g_region));
    aos_blk_virt_reset(v);
    aos_blk_client_bind(g_region, 0u, c);
    aos_blk_client_init_queues(c);
    aos_blk_storage_init(c->info, AOS_BLK_DISK_BLOCKS);
    aos_blk_virt_set_disk(v, g_region + AOS_BLK_DISK_OFF, AOS_BLK_DISK_BLOCKS);
    CHECK(aos_blk_virt_add_client(v, c) == 0);
    return 0;
}

static int enqueue_req(aos_blk_virt_client_t *c, aos_blk_req_code_t code,
                       uint64_t offset, uint64_t block, uint16_t count, uint32_t id)
{
    aos_blk_req_t req;
    uint32_t cap = c->capacity;

    if ((c->req->tail - c->req->head) == cap) {
        return -1;
    }
    memset(&req, 0, sizeof(req));
    req.code = code;
    req.io_or_offset = offset;
    req.block_number = block;
    req.count = count;
    req.id = id;
    c->req->buffers[c->req->tail % cap] = req;
    c->req->tail++;
    return 0;
}

static int dequeue_resp(aos_blk_virt_client_t *c, aos_blk_resp_t *out)
{
    uint32_t cap = c->capacity;
    if (c->resp->tail == c->resp->head) {
        return -1;
    }
    *out = c->resp->buffers[c->resp->head % cap];
    c->resp->head++;
    return 0;
}

static int test_abi_sizes(void)
{
    CHECK(sizeof(aos_blk_req_t) == 32u);
    CHECK(offsetof(aos_blk_req_t, io_or_offset) == 8u);
    CHECK(offsetof(aos_blk_req_t, count) == 24u);
    CHECK(sizeof(aos_blk_resp_t) == 12u);
    CHECK(offsetof(aos_blk_resp_t, id) == 8u);
    CHECK(offsetof(aos_blk_req_queue_t, plugged) == 8u);
    CHECK(offsetof(aos_blk_req_queue_t, buffers) == 16u);
    CHECK(offsetof(aos_blk_resp_queue_t, buffers) == 8u);
    CHECK(sizeof(aos_blk_storage_info_t) == 88u);
    CHECK(offsetof(aos_blk_storage_info_t, capacity) == 80u);
    CHECK(AOS_BLK_TRANSFER_SIZE == 4096u);
    CHECK(AOS_BLK_QUEUE_CAPACITY == 128u);
    CHECK(AOS_BLK_GUEST_MAX_SEGMENT_SIZE == 0x100000u);
    CHECK(AOS_BLK_DATA_CELLS == 257u);
    CHECK(AOS_VIRTIO_BLK_GUEST_IPA == 0x0A020000UL);
    CHECK(AOS_VIRTIO_BLK_GUEST_IPA != 0x0A000000UL);
    CHECK(AOS_VIRTIO_BLK_GUEST_IPA != AOS_VIRTIO_NET_GUEST_IPA);
    CHECK(AOS_VIRTIO_BLK_VIRQ == 52u);
    CHECK(AOS_VIRTIO_BLK_DTB_SPI == 20u);
    CHECK(AOS_VIRTIO_BLK_VIRQ != 48u);
    CHECK(AOS_VIRTIO_BLK_VIRQ != 49u);
    CHECK(AOS_VIRTIO_BLK_VIRQ != 50u);
    CHECK(AOS_VIRTIO_BLK_VIRQ != 51u);
    PASS("test_abi_sizes");
}

static int test_empty_pump(void)
{
    aos_blk_virt_t v;
    aos_blk_virt_client_t c;

    memset(g_region, 0, sizeof(g_region));
    aos_blk_virt_reset(&v);
    aos_blk_client_bind(g_region, 0u, &c);
    aos_blk_client_init_queues(&c);
    aos_blk_storage_init(c.info, AOS_BLK_DISK_BLOCKS);
    aos_blk_virt_set_disk(&v, g_region + AOS_BLK_DISK_OFF, AOS_BLK_DISK_BLOCKS);
    CHECK(aos_blk_virt_add_client(&v, &c) == 0);
    CHECK(aos_blk_virt_pump(&v) == 0u);
    CHECK(c.info->ready == true);
    CHECK(c.info->capacity == (uint64_t)AOS_BLK_DISK_BLOCKS);
    PASS("test_empty_pump");
}

static int test_write_read_roundtrip(void)
{
    aos_blk_virt_t v;
    aos_blk_virt_client_t c;
    uint8_t pattern[AOS_BLK_TRANSFER_SIZE];
    aos_blk_resp_t resp;
    uint32_t i;

    if (setup_one(&v, &c) != 0) {
        return 1;
    }
    for (i = 0; i < AOS_BLK_TRANSFER_SIZE; i++) {
        pattern[i] = (uint8_t)(i * 3u);
    }
    memcpy(c.data, pattern, AOS_BLK_TRANSFER_SIZE);

    CHECK(enqueue_req(&c, AOS_BLK_REQ_WRITE, 0, 3, 1, 11) == 0);
    CHECK(aos_blk_virt_pump(&v) == 1u);
    CHECK(dequeue_resp(&c, &resp) == 0);
    CHECK(resp.status == AOS_BLK_RESP_OK);
    CHECK(resp.id == 11u);
    CHECK(resp.success_count == 1u);
    CHECK(memcmp(g_region + AOS_BLK_DISK_OFF + 3u * AOS_BLK_TRANSFER_SIZE,
                 pattern, AOS_BLK_TRANSFER_SIZE) == 0);

    memset(c.data, 0, AOS_BLK_TRANSFER_SIZE);
    CHECK(enqueue_req(&c, AOS_BLK_REQ_READ, 0, 3, 1, 12) == 0);
    CHECK(aos_blk_virt_pump(&v) == 1u);
    CHECK(dequeue_resp(&c, &resp) == 0);
    CHECK(resp.status == AOS_BLK_RESP_OK);
    CHECK(resp.id == 12u);
    CHECK(memcmp(c.data, pattern, AOS_BLK_TRANSFER_SIZE) == 0);
    PASS("test_write_read_roundtrip");
}

static int test_flush_and_oob(void)
{
    aos_blk_virt_t v;
    aos_blk_virt_client_t c;
    aos_blk_resp_t resp;

    if (setup_one(&v, &c) != 0) {
        return 1;
    }
    CHECK(enqueue_req(&c, AOS_BLK_REQ_FLUSH, 0, 0, 0, 1) == 0);
    CHECK(aos_blk_virt_pump(&v) == 1u);
    CHECK(dequeue_resp(&c, &resp) == 0);
    CHECK(resp.status == AOS_BLK_RESP_OK);

    CHECK(enqueue_req(&c, AOS_BLK_REQ_READ, 0, AOS_BLK_DISK_BLOCKS, 1, 2) == 0);
    CHECK(aos_blk_virt_pump(&v) == 1u);
    CHECK(dequeue_resp(&c, &resp) == 0);
    CHECK(resp.status == AOS_BLK_RESP_ERR_INVALID_PARAM);

    CHECK(enqueue_req(&c, AOS_BLK_REQ_WRITE, AOS_BLK_DATA_BYTES, 0, 1, 3) == 0);
    CHECK(aos_blk_virt_pump(&v) == 1u);
    CHECK(dequeue_resp(&c, &resp) == 0);
    CHECK(resp.status == AOS_BLK_RESP_ERR_INVALID_PARAM);
    PASS("test_flush_and_oob");
}

static int test_two_clients_share_disk(void)
{
    aos_blk_virt_t v;
    aos_blk_virt_client_t a;
    aos_blk_virt_client_t b;
    uint8_t frame[64];
    aos_blk_resp_t resp;

    memset(g_region, 0, sizeof(g_region));
    memset(frame, 0x5a, sizeof(frame));
    aos_blk_virt_reset(&v);
    aos_blk_client_bind(g_region, 0u, &a);
    aos_blk_client_bind(g_region, 1u, &b);
    aos_blk_client_init_queues(&a);
    aos_blk_client_init_queues(&b);
    aos_blk_storage_init(a.info, AOS_BLK_DISK_BLOCKS);
    aos_blk_storage_init(b.info, AOS_BLK_DISK_BLOCKS);
    aos_blk_virt_set_disk(&v, g_region + AOS_BLK_DISK_OFF, AOS_BLK_DISK_BLOCKS);
    CHECK(aos_blk_virt_add_client(&v, &a) == 0);
    CHECK(aos_blk_virt_add_client(&v, &b) == 0);

    memcpy(a.data, frame, sizeof(frame));
    CHECK(enqueue_req(&a, AOS_BLK_REQ_WRITE, 0, 1, 1, 21) == 0);
    CHECK(aos_blk_virt_pump(&v) == 1u);
    CHECK(dequeue_resp(&a, &resp) == 0);
    CHECK(resp.status == AOS_BLK_RESP_OK);

    CHECK(enqueue_req(&b, AOS_BLK_REQ_READ, 0, 1, 1, 22) == 0);
    CHECK(aos_blk_virt_pump(&v) == 1u);
    CHECK(dequeue_resp(&b, &resp) == 0);
    CHECK(resp.status == AOS_BLK_RESP_OK);
    CHECK(memcmp(b.data, frame, sizeof(frame)) == 0);
    PASS("test_two_clients_share_disk");
}

static int test_drop_when_resp_full(void)
{
    aos_blk_virt_t v;
    aos_blk_virt_client_t c;
    uint32_t reqs_before;

    if (setup_one(&v, &c) != 0) {
        return 1;
    }
    /* Fill the response queue so the pump cannot complete. */
    c.resp->tail = c.resp->head + c.capacity;
    CHECK(enqueue_req(&c, AOS_BLK_REQ_FLUSH, 0, 0, 0, 99) == 0);
    reqs_before = c.req->tail - c.req->head;
    CHECK(reqs_before == 1u);
    CHECK(aos_blk_virt_pump(&v) == 0u);
    CHECK((c.req->tail - c.req->head) == 1u);
    PASS("test_drop_when_resp_full");
}

typedef struct {
    uint32_t calls;
    aos_blk_req_code_t code;
    uint16_t count;
} backend_probe_t;

static aos_blk_resp_status_t probe_backend(
    void *ctx, aos_blk_virt_client_t *client, const aos_blk_req_t *req)
{
    backend_probe_t *probe = (backend_probe_t *)ctx;
    probe->calls++;
    probe->code = req->code;
    probe->count = req->count;
    if (req->code == AOS_BLK_REQ_READ) {
        memset(client->data + (uint32_t)req->io_or_offset, 0xa5,
               (size_t)req->count * AOS_BLK_TRANSFER_SIZE);
    }
    return AOS_BLK_RESP_OK;
}

static int test_external_backend(void)
{
    aos_blk_virt_t v;
    aos_blk_virt_client_t c;
    aos_blk_resp_t resp;
    backend_probe_t probe = {0};

    if (setup_one(&v, &c) != 0) {
        return 1;
    }
    aos_blk_virt_set_backend(&v, probe_backend, &probe);
    CHECK(enqueue_req(&c, AOS_BLK_REQ_READ, 0, 42, 1, 123) == 0);
    CHECK(aos_blk_virt_pump(&v) == 1u);
    CHECK(dequeue_resp(&c, &resp) == 0);
    CHECK(resp.status == AOS_BLK_RESP_OK);
    CHECK(resp.success_count == 1u);
    CHECK(probe.calls == 1u);
    CHECK(probe.code == AOS_BLK_REQ_READ);
    CHECK(c.data[0] == 0xa5 && c.data[AOS_BLK_TRANSFER_SIZE - 1u] == 0xa5);
    PASS("test_external_backend");
}

static int test_maxphys_backend_request(void)
{
    aos_blk_virt_t v;
    aos_blk_virt_client_t c;
    aos_blk_resp_t resp;
    backend_probe_t probe = {0};

    if (setup_one(&v, &c) != 0) {
        return 1;
    }
    aos_blk_virt_set_backend(&v, probe_backend, &probe);
    CHECK(enqueue_req(&c, AOS_BLK_REQ_READ, 0, 42,
                      AOS_BLK_DATA_CELLS, 124) == 0);
    CHECK(aos_blk_virt_pump(&v) == 1u);
    CHECK(dequeue_resp(&c, &resp) == 0);
    CHECK(resp.status == AOS_BLK_RESP_OK);
    CHECK(resp.success_count == AOS_BLK_DATA_CELLS);
    CHECK(probe.calls == 1u);
    CHECK(probe.count == AOS_BLK_DATA_CELLS);
    CHECK(c.data[AOS_BLK_DATA_BYTES - 1u] == 0xa5);
    PASS("test_maxphys_backend_request");
}

int main(void)
{
    int failed = 0;

    printf("blk_virt_pump\n");
    failed += test_abi_sizes();
    failed += test_empty_pump();
    failed += test_write_read_roundtrip();
    failed += test_flush_and_oob();
    failed += test_two_clients_share_disk();
    failed += test_drop_when_resp_full();
    failed += test_external_backend();
    failed += test_maxphys_backend_request();
    if (failed) {
        printf("%d test(s) failed\n", failed);
        return 1;
    }
    printf("All blk_virt_pump tests passed\n");
    return 0;
}
