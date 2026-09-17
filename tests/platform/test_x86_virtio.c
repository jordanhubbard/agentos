#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <platform/x86_virtio.h>
#include <libvmm/virtio/virtio.h>
#include <libvmm/virtio/gpa.h>
#include <libvmm/virq.h>
#undef vprintf

const char vmm_pd_name[]="x86-virtio-test";
int printf_(const char *format, ...)
{
    va_list args; va_start(args,format);
    int result=vprintf(format,args); va_end(args); return result;
}
static unsigned config_reads, notifications;
static uint8_t ram[4096] __attribute__((aligned(4096)));
static void reset(virtio_device_t *dev)
{
    for (size_t i=0; i<dev->num_vqs; i++) virtio_queue_reset_guest_rings(&dev->vqs[i]);
}
static bool features(virtio_device_t *dev, uint32_t *value)
{ (void)dev; *value=0; return true; }
static bool driver_features(virtio_device_t *dev, uint32_t value)
{ dev->features_happy=value==0; return dev->features_happy; }
static bool config_read(virtio_device_t *dev, uint32_t offset, uint32_t *value)
{
    (void)dev; config_reads++;
    if (offset>=8) return false;
    *value=offset ? 0x88776655u : 0x44332211u; return true;
}
static bool config_write(virtio_device_t *dev, uint32_t offset, uint32_t value)
{ (void)dev; (void)offset; (void)value; return false; }
static bool notify(virtio_device_t *dev)
{
    notifications++; dev->regs.InterruptStatus |= 1;
    return virq_inject((int)dev->virq);
}
static uint32_t read_at(unsigned offset, unsigned width)
{
    uint32_t value=0;
    assert(aos_x86_virtio_access(AOS_X86_VIRTIO_BASE+offset,width,false,&value));
    return value;
}
static void write_at(unsigned offset, uint32_t value)
{ assert(aos_x86_virtio_access(AOS_X86_VIRTIO_BASE+offset,4,true,&value)); }
static void reject(uint64_t gpa, unsigned width, bool write)
{
    uint32_t value=0xabcdef01;
    unsigned before=config_reads;
    assert(!aos_x86_virtio_access(gpa,width,write,&value));
    assert(value==0xabcdef01 && config_reads==before);
}
int main(void)
{
    aos_x86_ioapic_t controller, other;
    assert(aos_x86_ioapic_init(&controller,1)); other=controller;
    const aos_x86_ioapic_t pristine=other;
    virtio_device_funs_t ops={reset,features,driver_features,config_read,config_write,notify};
    virtio_queue_handler_t queues[2]={0}, second_queues[2]={0};
    virtio_device_t dev={.transport_type=VIRTIO_TRANSPORT_MMIO,.funs=&ops,
        .vqs=queues,.num_vqs=2,.virq=AOS_X86_VIRTIO_GSI_BASE};
    dev.regs.DeviceID=VIRTIO_DEVICE_ID_CONSOLE;
    assert(!virtio_mmio_register_device(&dev,AOS_X86_VIRTIO_BASE,4096,dev.virq));
    assert(!aos_x86_virtio_init(NULL,ram,sizeof(ram)));
    assert(!aos_x86_virtio_init(&controller,NULL,sizeof(ram)));
    assert(!aos_x86_virtio_init(&controller,ram,0));
    assert(!aos_x86_virtio_init(&controller,ram+1,sizeof(ram)));
    assert(!aos_x86_virtio_init(&controller,ram,sizeof(ram)-1));
    assert(!aos_x86_virtio_init(&controller,ram,AOS_X86_VIRTIO_BASE+1));
    assert(aos_x86_virtio_init(&controller,ram,sizeof(ram)));
    assert(!aos_x86_virtio_init(&other,ram,sizeof(ram)));
    assert(virtio_gpa_to_hva(0,sizeof(ram))==ram);
    assert(virtio_gpa_to_hva(sizeof(ram)-1,1)==ram+sizeof(ram)-1);
    assert(!virtio_gpa_to_hva(sizeof(ram)-1,2));
    assert(!virtio_gpa_to_hva(UINT64_MAX,2));
    assert(!virtio_gpa_to_hva(AOS_X86_VIRTIO_BASE,1));
    assert(!virtio_mmio_register_device(&dev,AOS_X86_VIRTIO_BASE+1,4096,dev.virq));
    assert(!virtio_mmio_register_device(&dev,AOS_X86_VIRTIO_BASE,8192,dev.virq));
    assert(!virtio_mmio_register_device(&dev,AOS_X86_VIRTIO_BASE,4096,17));
    assert(virtio_mmio_register_device(&dev,AOS_X86_VIRTIO_BASE,4096,16));
    assert(!virtio_mmio_register_device(&dev,AOS_X86_VIRTIO_BASE,4096,16));
    virtio_device_t second=dev; second.vqs=second_queues; second.virq=17;
    assert(virtio_mmio_register_device(&second,AOS_X86_VIRTIO_BASE+4096,4096,17));
    assert(read_at(REG_VIRTIO_MMIO_MAGIC_VALUE,4)==VIRTIO_MMIO_DEV_MAGIC);
    assert(read_at(REG_VIRTIO_MMIO_DEVICE_ID,4)==VIRTIO_DEVICE_ID_CONSOLE);
    for (unsigned i=0; i<8; i++) assert(read_at(0x100+i,1)==0x11u*(i+1));
    assert(read_at(0x102,2)==0x4433);
    assert(read_at(0x104,4)==0x88776655);
    reject(AOS_X86_VIRTIO_BASE,1,false);
    reject(AOS_X86_VIRTIO_BASE+1,4,false);
    reject(AOS_X86_VIRTIO_BASE+0x101,2,false);
    reject(AOS_X86_VIRTIO_BASE+0x100,1,true);
    reject(AOS_X86_VIRTIO_BASE+0x100,8,false);
    reject(AOS_X86_VIRTIO_BASE+0x200,4,false);
    reject(AOS_X86_VIRTIO_BASE+8192,4,false);
    reject(UINT64_MAX,4,false);
    uint32_t value=123;
    assert(!aos_x86_virtio_access(AOS_X86_VIRTIO_BASE+0x108,4,false,&value) && value==123);
    write_at(REG_VIRTIO_MMIO_QUEUE_NUM,8);
    write_at(REG_VIRTIO_MMIO_QUEUE_DESC_LOW,256);
    write_at(REG_VIRTIO_MMIO_QUEUE_AVAIL_LOW,512);
    write_at(REG_VIRTIO_MMIO_QUEUE_USED_LOW,1024);
    write_at(REG_VIRTIO_MMIO_QUEUE_READY,1);
    assert(queues[0].ready && queues[0].virtq.desc==(void *)(ram+256));
    unsigned line=16;
    value=0x10+2*line; assert(aos_x86_ioapic_io(&controller,0,true,&value));
    value=(1u<<15)|0x50u; assert(aos_x86_ioapic_io(&controller,0x10,true,&value));
    write_at(REG_VIRTIO_MMIO_QUEUE_NOTIFY,0);
    assert(notifications==1 && controller.asserted==(1u<<line));
    aos_x86_ioapic_route_t route;
    assert(aos_x86_ioapic_route(&controller,line,&route) && route.vector==0x50 && route.level);
    assert(aos_x86_ioapic_accept(&controller,line));
    assert(!aos_x86_ioapic_route(&controller,line,&route));
    aos_x86_ioapic_eoi(&controller,0x50);
    assert(aos_x86_ioapic_route(&controller,line,&route)); /* still asserted */
    write_at(REG_VIRTIO_MMIO_INTERRUPT_ACK,1);
    assert(!controller.asserted && !aos_x86_ioapic_route(&controller,line,&route));
    assert(!virq_inject(15) && !virq_inject(24) && !virq_inject(18));
    assert(!virq_inject_vcpu(1,16));
    second.regs.InterruptStatus=1;
    assert(virq_inject_vcpu(0,17) && controller.asserted==(1u<<17));
    write_at(REG_VIRTIO_MMIO_QUEUE_NOTIFY,0);
    write_at(REG_VIRTIO_MMIO_STATUS,0);
    assert(!(controller.asserted & (1u<<16)) && (controller.asserted & (1u<<17)));
    assert(!queues[0].ready && !memcmp(&other,&pristine,sizeof(other)));
    puts("PASS: x86 virtio aperture, subword config, GPA mapping and IOAPIC level/ACK/reset");
    return 0;
}
