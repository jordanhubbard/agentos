#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <libvmm/virtio/virtio.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/gpa.h>
#undef vprintf

const char vmm_pd_name[] = "mmio-test";
int printf_(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int rc=vprintf(fmt, ap); va_end(ap); return rc;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned resets, notifications;
static uint8_t memory[4096];
#define GPA_BASE UINT64_C(0x123450000)
static void *translate(uint64_t gpa, size_t size)
{
    if (gpa<GPA_BASE || size>sizeof(memory) || gpa-GPA_BASE>sizeof(memory)-size) return NULL;
    return memory+(gpa-GPA_BASE);
}
static void reset(virtio_device_t *d)
{
    resets++;
    for (size_t i=0;i<d->num_vqs;i++) virtio_queue_reset_guest_rings(&d->vqs[i]);
}
static bool features(virtio_device_t *d, uint32_t *v)
{
    *v=d->regs.DeviceFeaturesSel ? 1u : 0u; return true;
}
static bool driver_features(virtio_device_t *d, uint32_t v)
{
    d->features_happy=(d->regs.DriverFeaturesSel==1 && v==1); return d->features_happy;
}
static bool config_read(virtio_device_t *d, uint32_t o, uint32_t *v)
{
    (void)d; *v=0xcafebabeu; return o==0;
}
static bool config_write(virtio_device_t *d, uint32_t o, uint32_t v)
{ (void)d; (void)o; (void)v; return false; }
static bool notify(virtio_device_t *d)
{
    if (d->regs.QueueNotify>=d->num_vqs) return false;
    notifications++; return true;
}
static void address(virtio_device_t *d, size_t lo, uint64_t value)
{
    CHECK(virtio_mmio_reg_write(d,lo,(uint32_t)value));
    CHECK(virtio_mmio_reg_write(d,lo+4,(uint32_t)(value>>32)));
}
int main(void)
{
    virtio_device_funs_t ops={reset,features,driver_features,config_read,config_write,notify};
    virtio_queue_handler_t queues[2]={0};
    virtio_device_t d={.transport_type=VIRTIO_TRANSPORT_MMIO,.funs=&ops,.vqs=queues,.num_vqs=2};
    d.regs.DeviceID=VIRTIO_DEVICE_ID_CONSOLE; d.regs.VendorID=VIRTIO_MMIO_DEV_VENDOR_ID;
    uint32_t v=0;
    CHECK(virtio_mmio_reg_read(&d,REG_VIRTIO_MMIO_MAGIC_VALUE,&v) && v==VIRTIO_MMIO_DEV_MAGIC);
    CHECK(virtio_mmio_reg_read(&d,REG_VIRTIO_MMIO_DEVICE_ID,&v) && v==VIRTIO_DEVICE_ID_CONSOLE);
    CHECK(virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_DEVICE_FEATURES_SEL,1));
    CHECK(virtio_mmio_reg_read(&d,REG_VIRTIO_MMIO_DEVICE_FEATURES,&v) && v==1);
    CHECK(virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL,1));
    CHECK(!virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_DRIVER_FEATURES,2));
    CHECK(virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_DRIVER_FEATURES,1));
    CHECK(virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_STATUS,1));
    CHECK(virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_STATUS,3));
    CHECK(virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_STATUS,11));
    CHECK(virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_STATUS,15));
    CHECK(virtio_mmio_reg_read(&d,REG_VIRTIO_MMIO_STATUS,&v) && v==15);
    CHECK(virtio_mmio_reg_read(&d,REG_VIRTIO_MMIO_CONFIG,&v) && v==0xcafebabeu);
    v=0x12345678;
    CHECK(!virtio_mmio_reg_read(&d,REG_VIRTIO_MMIO_CONFIG+4,&v) && v==0x12345678);
    CHECK(!virtio_mmio_reg_read(&d,0x200,&v) && v==0x12345678);
    CHECK(!virtio_mmio_reg_read(NULL,0,&v) && !virtio_mmio_reg_read(&d,0,NULL));
    virtio_gpa_set_translate(translate);
    CHECK(virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_QUEUE_SEL,0));
    CHECK(virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_QUEUE_NUM,8));
    address(&d,REG_VIRTIO_MMIO_QUEUE_DESC_LOW,GPA_BASE);
    address(&d,REG_VIRTIO_MMIO_QUEUE_AVAIL_LOW,GPA_BASE+512);
    address(&d,REG_VIRTIO_MMIO_QUEUE_USED_LOW,GPA_BASE+1024);
    CHECK(virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_QUEUE_READY,1));
    CHECK(queues[0].virtq.desc==(void *)memory && queues[0].virtq.avail==(void *)(memory+512));
    CHECK(queues[0].virtq.used==(void *)(memory+1024));
    CHECK(!virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_QUEUE_NUM,16));
    CHECK(!virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_QUEUE_DESC_LOW,0));
    CHECK(!virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_QUEUE_AVAIL_HIGH,0));
    CHECK(!virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_QUEUE_USED_LOW,0));
    CHECK(queues[0].virtq.num==8 && queues[0].virtq.desc==(void *)memory);
    CHECK(queues[0].virtq.avail==(void *)(memory+512) && queues[0].virtq.used==(void *)(memory+1024));
    CHECK(virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_QUEUE_READY,1));
    CHECK(virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_QUEUE_NOTIFY,0) && notifications==1);
    CHECK(!virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_QUEUE_NOTIFY,2) && notifications==1);
    d.regs.InterruptStatus=3;
    CHECK(virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_INTERRUPT_ACK,1));
    CHECK(virtio_mmio_reg_read(&d,REG_VIRTIO_MMIO_INTERRUPT_STATUS,&v) && v==2);
    CHECK(virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_QUEUE_SEL,2));
    CHECK(!virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_QUEUE_NUM,8));
    v=42; CHECK(!virtio_mmio_reg_read(&d,REG_VIRTIO_MMIO_QUEUE_READY,&v) && v==42);
    CHECK(virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_STATUS,0) && resets==1);
    CHECK(!queues[0].ready && queues[0].virtq.desc==NULL && d.regs.Status==0);
    CHECK(virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_QUEUE_SEL,1));
    CHECK(virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_QUEUE_NUM,8));
    address(&d,REG_VIRTIO_MMIO_QUEUE_DESC_LOW,UINT64_MAX-3);
    CHECK(!virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_QUEUE_READY,1) && !queues[1].ready);
    address(&d,REG_VIRTIO_MMIO_QUEUE_DESC_LOW,GPA_BASE+1);
    address(&d,REG_VIRTIO_MMIO_QUEUE_AVAIL_LOW,GPA_BASE+512);
    address(&d,REG_VIRTIO_MMIO_QUEUE_USED_LOW,GPA_BASE+1024);
    CHECK(!virtio_mmio_reg_write(&d,REG_VIRTIO_MMIO_QUEUE_READY,1) && !queues[1].ready);
    CHECK((uintptr_t)queues[1].virtq.desc==GPA_BASE+1);
    CHECK(!virtio_mmio_reg_write(&d,0x200,0));
    puts("PASS: shared libvmm register negotiation, GPA mapping, IRQ ack, reset and rejection");
    return 0;
}
