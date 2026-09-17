/* Exercise the production block backend with real guest and sDDF rings.
 * Only MMIO registration, notifications and interrupt delivery are mocked. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libvmm/virtio/block.h>
#include <libvmm/virtio/gpa.h>

static struct virtio_blk_device device;
static blk_queue_handle_t queue;
static uint8_t guest[65536], data[8192];
static struct virtq_desc descriptors[16];
static blk_storage_info_t info;
static unsigned interrupts, notifications;

void vmm_notify(seL4_CPtr channel) { assert(channel == 42u); notifications++; }
bool virq_inject(int irq) { assert(irq == 52); interrupts++; return true; }
bool virtio_mmio_register_device(virtio_device_t *dev, uintptr_t base,
                                uintptr_t size, size_t irq)
{
    assert(dev == &device.virtio_device && base == 0x1000u &&
           size == 0x1000u && irq == 52u);
    return true;
}
bool virtio_pci_register_device(virtio_device_t *dev, int irq)
{ (void)dev; (void)irq; abort(); }
bool virtio_pci_alloc_dev_cfg_space(virtio_device_t *dev, uint8_t slot)
{ (void)dev; (void)slot; abort(); }
bool virtio_pci_alloc_memory_bar(virtio_device_t *dev, uint8_t bar, uint32_t size)
{ (void)dev; (void)bar; (void)size; abort(); }

static void *translate(uint64_t gpa, size_t size)
{
    if (gpa > sizeof(guest) || size > sizeof(guest) - gpa) return NULL;
    return guest + gpa;
}

static void setup(void)
{
    memset(&device, 0, sizeof(device));
    memset(guest, 0xa5, sizeof(guest));
    memset(descriptors, 0, sizeof(descriptors));
    interrupts = notifications = 0u;
    info = (blk_storage_info_t){.capacity = 256, .sector_size = 512};
    blk_req_queue_t *req = calloc(1, sizeof(*req) + 8 * sizeof(blk_req_t));
    blk_resp_queue_t *resp = calloc(1, sizeof(*resp) + 8 * sizeof(blk_resp_t));
    assert(req && resp);
    blk_queue_init(&queue, req, resp, 8);
    assert(virtio_mmio_blk_init(&device, 0x1000, 0x1000, 52,
        (uintptr_t)data, sizeof(data), &info, &queue, 8, 42));
    struct virtq *ring = &device.vqs[0].virtq;
    ring->num = 16;
    ring->desc = descriptors;
    ring->avail = calloc(1, sizeof(*ring->avail) + 16 * sizeof(uint16_t));
    ring->used = calloc(1, sizeof(*ring->used) + 16 * sizeof(struct virtq_used_elem));
    assert(ring->avail && ring->used);
    device.vqs[0].ready = true;
    virtio_gpa_set_translate(translate);
    assert(!virtio_blk_is_quiesced(&device));
}

static void cleanup(void)
{
    free(queue.req_queue); free(queue.resp_queue);
    free(device.vqs[0].virtq.avail); free(device.vqs[0].virtq.used);
}

static void submit(unsigned slot, uint32_t type, uint64_t sector, uint32_t size)
{
    uint32_t base = slot * 16384u;
    struct virtio_blk_outhdr header = {.type = type, .sector = sector};
    memcpy(guest + base, &header, sizeof(header));
    unsigned head = 3u * slot;
    descriptors[head] = (struct virtq_desc){.addr = base, .len = sizeof(header),
        .flags = VIRTQ_DESC_F_NEXT, .next = head + 1u};
    descriptors[head + 1u] = (struct virtq_desc){.addr = base + sizeof(header),
        .len = size, .flags = VIRTQ_DESC_F_NEXT |
            (type == VIRTIO_BLK_T_IN ? VIRTQ_DESC_F_WRITE : 0u), .next = head + 2u};
    descriptors[head + 2u] = (struct virtq_desc){.addr = base + sizeof(header) + size,
        .len = 1u, .flags = VIRTQ_DESC_F_WRITE};
    struct virtq *ring = &device.vqs[0].virtq;
    ring->avail->ring[ring->avail->idx++ % ring->num] = head;
    assert(device.virtio_device.funs->queue_notify(&device.virtio_device));
}

static blk_req_t take(blk_req_code_t expected)
{
    blk_req_t req;
    assert(!blk_dequeue_req(&queue, &req.code, &req.io_or_offset,
        &req.block_number, &req.count, &req.id));
    assert(req.code == expected);
    assert(req.io_or_offset <= sizeof(data) &&
           req.count * 4096u <= sizeof(data) - req.io_or_offset);
    assert(!virtio_blk_is_quiesced(&device));
    return req;
}

static void complete(blk_req_t req, blk_resp_status_t status)
{
    assert(!blk_enqueue_resp(&queue, status, req.count, req.id));
    assert(!virtio_blk_is_quiesced(&device));
    assert(virtio_blk_handle_resp(&device));
}

static void test_chunked_read(void)
{
    setup();
    submit(0, VIRTIO_BLK_T_IN, 8, 12288);
    virtio_blk_begin_quiesce(&device);
    assert(!virtio_blk_is_quiesced(&device));
    /* Visible after the admission boundary: this chain must stay untouched. */
    submit(1, VIRTIO_BLK_T_IN, 0, 512);
    for (unsigned step = 0; step < 2; step++) {
        blk_req_t req = take(BLK_REQ_READ);
        assert(req.block_number == (step == 0 ? 1u : 3u));
        assert(req.count == (step == 0 ? 2u : 1u));
        for (unsigned i = 0; i < req.count * 4096u; i++)
            data[req.io_or_offset + i] = (req.block_number * 4096u + i) % 251u;
        complete(req, BLK_RESP_OK);
        assert(device.vqs[0].virtq.used->idx == step);
    }
    assert(virtio_blk_is_quiesced(&device));
    assert(device.vqs[0].last_idx == 1u && interrupts == 0u && notifications >= 2u);
    assert(device.vqs[0].virtq.used->ring[0].len == 12289u);
    for (unsigned i = 0; i < 12288u; i++)
        assert(guest[sizeof(struct virtio_blk_outhdr) + i] == (4096u + i) % 251u);
    assert(guest[sizeof(struct virtio_blk_outhdr) + 12288u] == VIRTIO_BLK_S_OK);
    assert(guest[16384u + sizeof(struct virtio_blk_outhdr)] == 0xa5u);
    /* Admission remains stopped even if guest rings are no longer mapped. */
    struct virtq_avail *saved = device.vqs[0].virtq.avail;
    device.vqs[0].virtq.avail = NULL;
    assert(device.virtio_device.funs->queue_notify(&device.virtio_device));
    device.vqs[0].virtq.avail = saved;
    cleanup();
}

static void test_overlapping_rmw(void)
{
    setup();
    memset(guest + sizeof(struct virtio_blk_outhdr), 0x31, 512);
    memset(guest + 16384u + sizeof(struct virtio_blk_outhdr), 0x62, 512);
    submit(0, VIRTIO_BLK_T_OUT, 1, 512);
    submit(1, VIRTIO_BLK_T_OUT, 1, 512);
    virtio_blk_begin_quiesce(&device);
    uint8_t disk[4096]; memset(disk, 0x99, sizeof(disk));
    for (unsigned step = 0; step < 2; step++) {
        blk_req_t read = take(BLK_REQ_READ);
        assert(read.count == 1 && read.block_number == 0);
        memcpy(data + read.io_or_offset, disk, sizeof(disk));
        complete(read, BLK_RESP_OK);
        assert(device.vqs[0].virtq.used->idx == step);
        blk_req_t write = take(BLK_REQ_WRITE);
        assert(write.count == 1 && write.block_number == 0);
        memcpy(disk, data + write.io_or_offset, sizeof(disk));
        for (unsigned i = 0; i < sizeof(disk); i++)
            assert(disk[i] == (i >= 512 && i < 1024 ? (step ? 0x62 : 0x31) : 0x99));
        complete(write, BLK_RESP_OK);
    }
    assert(virtio_blk_is_quiesced(&device));
    assert(device.vqs[0].virtq.used->idx == 2 && interrupts == 0);
    cleanup();
}

static void test_error_completion(void)
{
    setup();
    submit(0, VIRTIO_BLK_T_IN, 0, 512);
    virtio_blk_begin_quiesce(&device);
    complete(take(BLK_REQ_READ), BLK_RESP_ERR_UNSPEC);
    assert(virtio_blk_is_quiesced(&device));
    assert(guest[sizeof(struct virtio_blk_outhdr)] == 0xa5);
    assert(guest[sizeof(struct virtio_blk_outhdr) + 512] == VIRTIO_BLK_S_IOERR);
    assert(device.vqs[0].virtq.used->idx == 1);
    cleanup();
}

static void test_failed_predecessor(void)
{
    setup();
    memset(guest + sizeof(struct virtio_blk_outhdr), 0x31, 512);
    memset(guest + 16384u + sizeof(struct virtio_blk_outhdr), 0x62, 512);
    submit(0, VIRTIO_BLK_T_OUT, 1, 512);
    submit(1, VIRTIO_BLK_T_OUT, 1, 512);
    virtio_blk_begin_quiesce(&device);
    complete(take(BLK_REQ_READ), BLK_RESP_ERR_UNSPEC);
    assert(guest[sizeof(struct virtio_blk_outhdr) + 512] == VIRTIO_BLK_S_IOERR);
    assert(!blk_queue_empty_req(&queue));
    blk_req_t read = take(BLK_REQ_READ);
    memset(data + read.io_or_offset, 0x99, 4096);
    complete(read, BLK_RESP_OK);
    blk_req_t write = take(BLK_REQ_WRITE);
    for (unsigned i = 0; i < 4096; i++)
        assert(data[write.io_or_offset + i] == (i >= 512 && i < 1024 ? 0x62 : 0x99));
    complete(write, BLK_RESP_OK);
    assert(virtio_blk_is_quiesced(&device));
    assert(device.vqs[0].virtq.used->idx == 2);
    assert(guest[16384u + sizeof(struct virtio_blk_outhdr) + 512] == VIRTIO_BLK_S_OK);
    cleanup();
}

int main(void)
{
    test_chunked_read();
    test_overlapping_rmw();
    test_error_completion();
    test_failed_predecessor();
    puts("PASS: production block drain, chunked reads, overlapping RMW, errors and stopped admission");
}
