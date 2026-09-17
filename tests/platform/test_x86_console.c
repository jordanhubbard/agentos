#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <platform/x86_virtio.h>
#include <platform/vmm_virtio_console.h>
#include <libvmm/virtio/virtio.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/virtq.h>
#undef vprintf

const char vmm_pd_name[] = "x86-console-test";
int printf_(const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int n=vprintf(fmt,ap); va_end(ap); return n;
}
/* This configuration uses local device queues, not a notification cap. */
void seL4_Signal(seL4_CPtr cap) { (void)cap; assert(!"unexpected kernel notification"); }
static _Alignas(4096) unsigned char ram[0x20000];
static void write_reg(unsigned offset, uint32_t value)
{
    assert(aos_x86_virtio_access(AOS_X86_VIRTIO_BASE+offset,4,true,&value));
}
static uint32_t read_reg(unsigned offset)
{
    uint32_t value=0;
    assert(aos_x86_virtio_access(AOS_X86_VIRTIO_BASE+offset,4,false,&value));
    return value;
}
static void queue(unsigned index, unsigned base)
{
    write_reg(REG_VIRTIO_MMIO_QUEUE_SEL,index);
    write_reg(REG_VIRTIO_MMIO_QUEUE_NUM,QUEUE_SIZE);
    write_reg(REG_VIRTIO_MMIO_QUEUE_DESC_LOW,base);
    write_reg(REG_VIRTIO_MMIO_QUEUE_AVAIL_LOW,base+0x2000);
    write_reg(REG_VIRTIO_MMIO_QUEUE_USED_LOW,base+0x3000);
    write_reg(REG_VIRTIO_MMIO_QUEUE_READY,1);
}
int main(void)
{
    aos_x86_ioapic_t ioapic;
    assert(aos_x86_ioapic_init(&ioapic,1));
    assert(aos_x86_virtio_init(&ioapic,ram,sizeof(ram)));
    assert(!aos_vmm_virtio_console_init_at(AOS_X86_VIRTIO_BASE+1,16));
    assert(!aos_vmm_virtio_console_init_at(AOS_X86_VIRTIO_BASE,17));
    assert(aos_vmm_virtio_console_init_at(AOS_X86_VIRTIO_BASE,16));
    assert(read_reg(REG_VIRTIO_MMIO_DEVICE_ID)==VIRTIO_DEVICE_ID_CONSOLE);
    write_reg(REG_VIRTIO_MMIO_STATUS,1);
    write_reg(REG_VIRTIO_MMIO_STATUS,3);
    write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL,1);
    write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES,1);
    write_reg(REG_VIRTIO_MMIO_STATUS,11);
    queue(0,0); queue(1,0x8000);
    write_reg(REG_VIRTIO_MMIO_STATUS,15);
    aos_vmm_virtio_console_after_fault();
    assert(aos_vmm_virtio_console_driver_ready());

    const char output[]="guest console output";
    memcpy(ram+0x10000,output,sizeof(output));
    struct virtq_desc *tx=(void *)(ram+0x8000);
    tx[0]=(struct virtq_desc){.addr=0x10000,.len=sizeof(output)};
    struct virtq_avail *tx_avail=(void *)(ram+0xa000);
    tx_avail->ring[0]=0; tx_avail->idx=1;
    write_reg(REG_VIRTIO_MMIO_QUEUE_NOTIFY,1);
    struct virtq_used *tx_used=(void *)(ram+0xb000);
    assert(tx_used->idx==1 && tx_used->ring[0].id==0);
    assert(ioapic.asserted==(1u<<16));
    /* Rebinding must preserve pending output and the active device. */
    assert(!aos_vmm_virtio_console_init_at(AOS_X86_VIRTIO_BASE+4096,17));
    unsigned char actual[64];
    assert(aos_vmm_virtio_console_drain_tx(actual,sizeof(actual))==sizeof(output));
    assert(!memcmp(actual,output,sizeof(output)));
    assert(!aos_vmm_virtio_console_drain_tx(actual,sizeof(actual)));
    write_reg(REG_VIRTIO_MMIO_INTERRUPT_ACK,1);
    assert(!ioapic.asserted);

    struct virtq_desc *rx=(void *)ram;
    rx[0]=(struct virtq_desc){.addr=0x11000,.len=64,.flags=VIRTQ_DESC_F_WRITE};
    struct virtq_avail *rx_avail=(void *)(ram+0x2000);
    rx_avail->ring[0]=0; rx_avail->idx=1;
    const uint8_t input[]="host console input";
    assert(aos_vmm_virtio_console_push_rx_bytes(input,sizeof(input)));
    struct virtq_used *rx_used=(void *)(ram+0x3000);
    assert(rx_used->idx==1 && rx_used->ring[0].id==0 && rx_used->ring[0].len==sizeof(input));
    assert(!memcmp(ram+0x11000,input,sizeof(input)));
    assert(ioapic.asserted==(1u<<16));
    write_reg(REG_VIRTIO_MMIO_INTERRUPT_ACK,1);
    assert(!ioapic.asserted);
    write_reg(REG_VIRTIO_MMIO_STATUS,0);
    assert(!aos_vmm_virtio_console_driver_ready());
    assert(!aos_vmm_virtio_console_push_rx_bytes(input,sizeof(input)));
    puts("PASS: shared console backend TX/RX through x86 MMIO and IOAPIC");
}
