/* Exercise the actual MMIO dispatcher; only architecture fault access and
 * interrupt registration are stubbed for the host. */
#include <libvmm/virtio/virtio.h>
#include <libvmm/arch/aarch64/fault.h>
#include <libvmm/virq.h>
#include <assert.h>
#include <stdio.h>

bool fault_is_read(uint64_t fsr) { return fsr==0; }
uint64_t fault_get_data_mask(uint64_t offset,uint64_t fsr) { (void)offset;(void)fsr;return UINT32_MAX; }
uint64_t fault_get_data(seL4_UserContext *r,uint64_t fsr) { (void)fsr;return r->x0; }
void fault_emulate_write(seL4_UserContext *r,size_t offset,size_t fsr,size_t value)
{ (void)offset;(void)fsr;r->x0=value; }
bool fault_register_vm_exception_handler(uintptr_t base,size_t size,vm_exception_handler_t cb,void *data)
{ (void)base;(void)size;(void)cb;(void)data;return true; }
static virq_ack_fn_t registered_ack;
static void *registered_cookie;
static unsigned injected;
bool virq_register(size_t cpu,size_t irq,virq_ack_fn_t ack,void *data)
{ (void)cpu;(void)irq;registered_ack=ack;registered_cookie=data;return true; }
bool virq_inject_vcpu(size_t cpu,int irq)
{ assert(cpu==0 && irq==54);++injected;return true; }
bool virtio_queue_map_guest_rings(struct virtq *q) { (void)q;return true; }
static uint32_t config_offset,config_value;
static bool set_config(virtio_device_t *d,uint32_t offset,uint32_t value)
{ (void)d;config_offset=offset;config_value=value;return true; }
int main(void)
{
    virtio_queue_handler_t queue={.virtq={.used=(void *)(uintptr_t)0x12340000}};
    virtio_device_funs_t functions={.set_device_config=set_config};
    virtio_device_t device={.vqs=&queue,.num_vqs=1,.funs=&functions};
    seL4_UserContext regs={0};
    const uint32_t ids[]={0,1,UINT32_MAX};
    for (unsigned i=0;i<3;++i) {
        regs.x0=ids[i];
        assert(virtio_mmio_fault_handle(0,REG_VIRTIO_MMIO_SHM_SEL,1,&regs,&device));
        assert(queue.virtq.used==(void *)(uintptr_t)0x12340000);
        for (unsigned off=REG_VIRTIO_MMIO_SHM_LEN_LOW;off<=REG_VIRTIO_MMIO_SHM_BASE_HIGH;off+=4) {
            assert(virtio_mmio_fault_handle(0,off,0,&regs,&device));
            assert(regs.x0==UINT32_MAX);
        }
    }
    regs.x0=7;
    assert(virtio_mmio_fault_handle(0,REG_VIRTIO_MMIO_CONFIG+4,1,&regs,&device));
    assert(config_offset==4 && config_value==7);
    regs.x0=2;
    assert(virtio_mmio_fault_handle(0,REG_VIRTIO_MMIO_QUEUE_USED_HIGH,1,&regs,&device));
    assert((uintptr_t)queue.virtq.used==UINT64_C(0x212340000));
    device.transport_type=VIRTIO_TRANSPORT_MMIO;
    device.virq=54;
    assert(virtio_mmio_register_device(&device,0xa040000,0x1000,54));
    assert(registered_ack && registered_cookie==&device);
    device.regs.InterruptStatus=1;
    regs.x0=1;
    assert(virtio_mmio_fault_handle(0,REG_VIRTIO_MMIO_INTERRUPT_ACK,1,&regs,&device));
    assert(device.regs.InterruptStatus==0);
    /* A new completion arrives after device ACK but before GIC EOI. */
    device.regs.InterruptStatus=1;
    registered_ack(0,54,registered_cookie);
    assert(injected==1 && device.regs.InterruptStatus==1);
    assert(virtio_mmio_fault_handle(0,REG_VIRTIO_MMIO_INTERRUPT_ACK,1,&regs,&device));
    registered_ack(0,54,registered_cookie);
    assert(injected==1); /* Cleared device level must not storm. */
    device.regs.InterruptStatus=3;
    assert(virtio_mmio_fault_handle(0,REG_VIRTIO_MMIO_INTERRUPT_ACK,1,&regs,&device));
    registered_ack(0,54,registered_cookie);
    assert(injected==2 && device.regs.InterruptStatus==2);
    regs.x0=2;
    assert(virtio_mmio_fault_handle(0,REG_VIRTIO_MMIO_INTERRUPT_ACK,1,&regs,&device));
    registered_ack(0,54,registered_cookie);
    assert(injected==2);
    puts("PASS: shared-memory probes, relative config writes, persistent IRQ level and partial ACK");
    return 0;
}
