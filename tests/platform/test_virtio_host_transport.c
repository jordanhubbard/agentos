#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <platform/virtio_host_transport.h>
#include <sddf/virtio/transport/pci.h>
#include "virtio_blk.h"

static _Alignas(8) uint32_t mmio[128];
static _Alignas(8) unsigned char common_mem[64];
static _Alignas(8) uint32_t config[8];
static _Alignas(8) uint16_t notify[16];
#define MMIO(reg) mmio[(reg) / 4u]

static void test_mmio(void)
{
    aos_virtio_host_t t;
    assert(!aos_virtio_host_mmio(&t, (uintptr_t)mmio, sizeof(mmio), 2u));
    assert(!aos_virtio_host_notify(&t));
    MMIO(VIRTIO_MMIO_MAGIC_VALUE) = VIRTIO_MMIO_MAGIC;
    MMIO(VIRTIO_MMIO_VERSION) = 2;
    MMIO(VIRTIO_MMIO_DEVICE_ID) = 2;
    assert(!aos_virtio_host_mmio(&t, (uintptr_t)mmio + 1, sizeof(mmio), 2));
    assert(!aos_virtio_host_mmio(&t, (uintptr_t)mmio, 0xff, 2));
    assert(!aos_virtio_host_mmio(&t, (uintptr_t)mmio, sizeof(mmio), 1));
    assert(aos_virtio_host_mmio(&t, (uintptr_t)mmio, sizeof(mmio), 2));
    MMIO(VIRTIO_MMIO_DEVICE_FEATURES) = 0x42;
    assert(aos_virtio_host_features(&t, 1) == 0x42);
    assert(MMIO(VIRTIO_MMIO_DEVICE_FEATURES_SEL) == 1);
    aos_virtio_host_set_features(&t, 1, 0x91);
    assert(MMIO(VIRTIO_MMIO_DRIVER_FEATURES_SEL) == 1);
    assert(MMIO(VIRTIO_MMIO_DRIVER_FEATURES) == 0x91);
    aos_virtio_host_set_status(&t, 0xb);
    assert(aos_virtio_host_status(&t) == 0xb);
    MMIO(VIRTIO_MMIO_QUEUE_NUM_MAX) = 4;
    assert(!aos_virtio_host_queue(&t, 0, 8, 0x1000, 0x2000, 0x3000));
    assert(MMIO(VIRTIO_MMIO_QUEUE_READY) == 0);
    MMIO(VIRTIO_MMIO_QUEUE_NUM_MAX) = 8;
    assert(!aos_virtio_host_queue(&t, 0, 3, 0x1000, 0x2000, 0x3000));
    assert(!aos_virtio_host_queue(&t, 0, 8, 0x1001, 0x2000, 0x3000));
    assert(aos_virtio_host_queue(&t, 0, 8, UINT64_C(0x12300001000),
                                  UINT64_C(0x23400002000), UINT64_C(0x34500003000)));
    assert(MMIO(VIRTIO_MMIO_QUEUE_DESC_LOW) == 0x1000);
    assert(MMIO(VIRTIO_MMIO_QUEUE_DESC_HIGH) == 0x123);
    assert(MMIO(VIRTIO_MMIO_QUEUE_AVAIL_HIGH) == 0x234);
    assert(MMIO(VIRTIO_MMIO_QUEUE_USED_HIGH) == 0x345);
    assert(MMIO(VIRTIO_MMIO_QUEUE_NUM) == 8);
    assert(MMIO(VIRTIO_MMIO_QUEUE_READY) == 1);
    assert(!aos_virtio_host_queue(&t, 1, 8, 0x1000, 0x2000, 0x3000));
    MMIO(VIRTIO_MMIO_QUEUE_NOTIFY) = 0xffff;
    assert(aos_virtio_host_notify(&t));
    assert(MMIO(VIRTIO_MMIO_QUEUE_NOTIFY) == 0);
    MMIO(VIRTIO_MMIO_CONFIG) = 0xdeadbeef;
    MMIO(VIRTIO_MMIO_CONFIG + 4) = 0x123;
    uint64_t capacity = 0;
    assert(aos_virtio_host_config64(&t, 0, &capacity));
    assert(capacity == UINT64_C(0x123deadbeef));
    assert(!aos_virtio_host_config64(&t, 252, &capacity));
    aos_virtio_host_set_status(&t, 0);
    assert(!aos_virtio_host_notify(&t));
}

static bool bind_pci(aos_virtio_host_t *t, uint32_t multiplier)
{
    return aos_virtio_host_pci(t, (uintptr_t)common_mem, 56u,
                               (uintptr_t)config, sizeof(config),
                               (uintptr_t)notify, sizeof(notify), multiplier);
}

static void test_pci(void)
{
    aos_virtio_host_t t;
    /* Use the upstream sDDF wire struct as an independent layout oracle. */
    virtio_pci_common_cfg_t *c = (virtio_pci_common_cfg_t *)common_mem;
    memset(common_mem, 0, sizeof(common_mem));
    for (unsigned i = 0; i < 16; i++) notify[i] = 0xa55a;
    assert(!aos_virtio_host_pci(&t, (uintptr_t)common_mem, 55,
                                 (uintptr_t)config, sizeof(config),
                                 (uintptr_t)notify, sizeof(notify), 4));
    assert(!aos_virtio_host_notify(&t));
    assert(!aos_virtio_host_pci(&t, UINTPTR_MAX - 3u, 56,
                                 (uintptr_t)config, sizeof(config),
                                 (uintptr_t)notify, sizeof(notify), 4));
    assert(bind_pci(&t, 4));
    c->device_feature = 0x55;
    assert(aos_virtio_host_features(&t, 1) == 0x55);
    assert(c->device_feature_select == 1);
    aos_virtio_host_set_features(&t, 1, 0x12);
    assert(c->driver_feature_select == 1 && c->driver_feature == 0x12);
    c->config_generation = 0xa5;
    c->queue_select = 0x5a5a;
    aos_virtio_host_set_status(&t, 0xf);
    assert(aos_virtio_host_status(&t) == 0xf);
    assert(c->config_generation == 0xa5 && c->queue_select == 0x5a5a);
    c->num_queues = 2;
    c->queue_size = 8;
    c->queue_notify_off = 3;
    assert(!aos_virtio_host_queue(&t, 2, 8, 0x1000, 0x2000, 0x3000));
    assert(!aos_virtio_host_queue(&t, 1, 16, 0x1000, 0x2000, 0x3000));
    assert(c->queue_enable == 0 && c->queue_desc == 0);
    assert(aos_virtio_host_queue(&t, 1, 8, UINT64_C(0x12300001000),
                                  UINT64_C(0x23400002000), UINT64_C(0x34500003000)));
    assert(c->queue_select == 1 && c->queue_size == 8 && c->queue_enable == 1);
    assert(c->config_msix_vector == UINT16_MAX && c->queue_msix_vector == UINT16_MAX);
    assert(c->queue_desc == UINT64_C(0x12300001000));
    assert(c->queue_driver == UINT64_C(0x23400002000));
    assert(c->queue_device == UINT64_C(0x34500003000));
    assert(aos_virtio_host_notify(&t));
    for (unsigned i = 0; i < 16; i++) assert(notify[i] == (i == 6 ? 1 : 0xa55a));
    config[0] = 0x76543210;
    config[1] = 0xfedcba98;
    uint64_t value = 0;
    uint32_t word = 0;
    assert(aos_virtio_host_config64(&t, 0, &value));
    assert(value == UINT64_C(0xfedcba9876543210));
    assert(aos_virtio_host_config32(&t, 4, &word) && word == 0xfedcba98);
    assert(!aos_virtio_host_config32(&t, 1, &word));
    assert(!aos_virtio_host_config32(&t, sizeof(config), &word));
    assert(!aos_virtio_host_config64(&t, sizeof(config) - 4, &value));
    aos_virtio_host_set_status(&t, 0);
    assert(!aos_virtio_host_notify(&t));
    /* Model completed hardware reset before testing another binding. */
    c->queue_enable = 0;
    c->queue_desc = 0;
    assert(bind_pci(&t, UINT32_MAX));
    assert(!aos_virtio_host_queue(&t, 0, 8, 0x1000, 0x2000, 0x3000));
    assert(c->queue_enable == 0 && c->queue_desc == 0);
    assert(bind_pci(&t, 1));
    assert(!aos_virtio_host_queue(&t, 0, 8, 0x1000, 0x2000, 0x3000));
    assert(bind_pci(&t, 0)); /* zero multiplier is permitted by Virtio */
    assert(aos_virtio_host_queue(&t, 0, 8, 0x1000, 0x2000, 0x3000));
    assert(aos_virtio_host_notify(&t) && notify[0] == 0);
}

static void test_multiple_queues(void)
{
    aos_virtio_host_t t;
    aos_virtio_host_queue_t rx = {0}, tx = {0};
    memset(mmio, 0, sizeof(mmio));
    MMIO(VIRTIO_MMIO_MAGIC_VALUE)=VIRTIO_MMIO_MAGIC;
    MMIO(VIRTIO_MMIO_VERSION)=2;
    MMIO(VIRTIO_MMIO_DEVICE_ID)=1;
    MMIO(VIRTIO_MMIO_QUEUE_NUM_MAX)=8;
    assert(aos_virtio_host_mmio(&t,(uintptr_t)mmio,sizeof(mmio),1));
    assert(!aos_virtio_host_queue_notify(&t,&rx));
    assert(aos_virtio_host_queue_bind(&t,&rx,0,8,0x1000,0x2000,0x3000));
    assert(!aos_virtio_host_queue_bind(&t,&tx,0,8,0x4000,0x5000,0x6000));
    assert(!tx.owner);
    /* The fixture supplies the register bank selected for queue 1. */
    MMIO(VIRTIO_MMIO_QUEUE_READY)=0;
    assert(aos_virtio_host_queue_bind(&t,&tx,1,8,0x4000,0x5000,0x6000));
    assert(MMIO(VIRTIO_MMIO_QUEUE_SEL)==1 && MMIO(VIRTIO_MMIO_QUEUE_DESC_LOW)==0x4000);
    assert(aos_virtio_host_queue_notify(&t,&rx) && MMIO(VIRTIO_MMIO_QUEUE_NOTIFY)==0);
    assert(aos_virtio_host_queue_notify(&t,&tx) && MMIO(VIRTIO_MMIO_QUEUE_NOTIFY)==1);
    aos_virtio_host_t other=t;
    assert(!aos_virtio_host_queue_notify(&other,&tx));
    aos_virtio_host_set_status(&t,0);
    assert(!aos_virtio_host_queue_notify(&t,&rx) && !aos_virtio_host_queue_notify(&t,&tx));

    memset(common_mem,0,sizeof(common_mem));
    virtio_pci_common_cfg_t *c=(virtio_pci_common_cfg_t *)common_mem;
    c->num_queues=2; c->queue_size=8; c->queue_notify_off=3;
    for (unsigned i=0; i<16; i++) notify[i]=0xa55a;
    assert(bind_pci(&t,4));
    assert(aos_virtio_host_queue_bind(&t,&rx,0,8,0x1000,0x2000,0x3000));
    /* Supply queue 1's independent capability-bank values. */
    c->queue_enable=0; c->queue_size=8; c->queue_notify_off=5;
    assert(aos_virtio_host_queue_bind(&t,&tx,1,8,0x4000,0x5000,0x6000));
    assert(c->queue_select==1 && c->queue_desc==0x4000);
    assert(aos_virtio_host_queue_notify(&t,&tx) && notify[10]==1);
    assert(aos_virtio_host_queue_notify(&t,&rx) && notify[6]==0);
    for (unsigned i=0; i<16; i++) if (i!=6 && i!=10) assert(notify[i]==0xa55a);
    aos_virtio_host_queue_t saved=rx;
    assert(!aos_virtio_host_queue_bind(&t,&rx,2,8,0x7000,0x8000,0x9000));
    assert(rx.owner==saved.owner && rx.epoch==saved.epoch &&
           rx.index==saved.index && rx.notify_offset==saved.notify_offset);
    assert(aos_virtio_host_queue_notify(&t,&rx));
    aos_virtio_host_set_status(&t,0);
    assert(!aos_virtio_host_queue_notify(&t,&rx) && !aos_virtio_host_queue_notify(&t,&tx));
    /* Reset invalidates the old handle even after a new queue is enabled. */
    c->queue_enable=0; c->queue_size=8;
    assert(aos_virtio_host_queue_bind(&t,&rx,0,8,0x1000,0x2000,0x3000));
    assert(aos_virtio_host_queue_notify(&t,&rx));
    assert(!aos_virtio_host_queue_notify(&t,&saved));
}

int main(void)
{
    test_mmio();
    test_pci();
    test_multiple_queues();
    puts("PASS: host virtio MMIO/PCI registers, queue bounds, DMA addresses and notifications");
    return 0;
}
