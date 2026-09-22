#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libvmm/virtio/gpa.h>
#include <libvmm/virtio/config.h>
#include <platform/virtio_net_rx_accounting.h>

static uint8_t guest[256], packet[32];
static unsigned interrupts;
bool virq_inject(int irq) { assert(irq == 52); ++interrupts; return true; }
static void *translate(uint64_t gpa, size_t size)
{
    return gpa <= sizeof(guest) && size <= sizeof(guest) - gpa ? guest + gpa : NULL;
}
int main(void)
{
    struct virtio_net_device net = {0};
    net.virtio_device.device_data = &net;
    net.virtio_device.vqs = net.vqs;
    net.virtio_device.virq = 52;
    net.rx_data = packet;
    net_queue_t *freeq = calloc(1, sizeof(*freeq) + 8 * sizeof(net_buff_desc_t));
    net_queue_t *active = calloc(1, sizeof(*active) + 8 * sizeof(net_buff_desc_t));
    assert(freeq && active);
    net_queue_init(&net.rx, freeq, active, 8);
    struct virtq *vq = &net.vqs[0].virtq;
    vq->num = 8;
    vq->desc = calloc(8, sizeof(*vq->desc));
    vq->avail = calloc(1, sizeof(*vq->avail) + 8 * sizeof(uint16_t));
    vq->used = calloc(1, sizeof(*vq->used) + 8 * sizeof(struct virtq_used_elem));
    assert(vq->desc && vq->avail && vq->used);
    vq->desc[0] = (struct virtq_desc){.addr=64, .len=128, .flags=VIRTQ_DESC_F_WRITE};
    memset(guest, 0xa5, sizeof(guest));
    memset(packet, 0x3c, sizeof(packet));
    virtio_gpa_set_translate(translate);
    net_buff_desc_t buffer = {.io_or_offset=0, .len=sizeof(packet)};
    assert(net_enqueue_active(&net.rx, buffer) == 0);
    for (unsigned i = 0; i < 8; ++i)
        assert(aos_virtio_net_rx_completed(&net) == 0 && net_queue_length(active) == 1);
    net.virtio_device.regs.Status = VIRTIO_CONFIG_S_DRIVER_OK | VIRTIO_CONFIG_S_FEATURES_OK;
    assert(aos_virtio_net_rx_completed(&net) == 0 && net_queue_length(active) == 1);
    net.vqs[0].ready = true;
    /* No guest buffers: the backend drops the packet, never a completion. */
    assert(aos_virtio_net_rx_completed(&net) == 0 && net_queue_empty_active(&net.rx));
    assert(!interrupts && guest[64] == 0xa5 && vq->used->idx == 0);
    for (unsigned wrap = 0; wrap < 2; ++wrap) {
        assert(net_dequeue_free(&net.rx, &buffer) == 0);
        buffer.len = sizeof(packet);
        assert(net_enqueue_active(&net.rx, buffer) == 0);
        uint16_t before = wrap ? UINT16_MAX : 0;
        net.vqs[0].last_idx = before;
        vq->used->idx = before;
        vq->avail->ring[before % 8] = 0;
        vq->avail->idx = (uint16_t)(before + 1);
        assert(aos_virtio_net_rx_completed(&net) == 1);
        assert(vq->used->idx == (uint16_t)(before + 1));
        assert(vq->used->ring[before % 8].len == sizeof(struct virtio_net_hdr_mrg_rxbuf) + sizeof(packet));
        assert(memcmp(guest + 64 + sizeof(struct virtio_net_hdr_mrg_rxbuf), packet, sizeof(packet)) == 0);
        assert(aos_virtio_net_rx_completed(&net) == 0 && interrupts == wrap + 1);
    }
    assert(net_dequeue_free(&net.rx, &buffer) == 0);
    buffer.len = sizeof(packet);
    assert(net_enqueue_active(&net.rx, buffer) == 0);
    net.quiesced = true;
    assert(aos_virtio_net_rx_completed(&net) == 0 && net_queue_length(active) == 1);
    free(vq->desc); free(vq->avail); free(vq->used); free(freeq); free(active);
    puts("PASS: RX accounting counts published guest chains, excludes backlog/drops and handles wrap");
}
