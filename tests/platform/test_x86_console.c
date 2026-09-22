#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <platform/serial_layout.h>
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
static unsigned char *ram;
enum { RAM_BYTES = 0x40000 };
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
    ram=mmap(NULL,RAM_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(ram!=MAP_FAILED);
    assert(aos_vmm_virtio_console_quiesce());
    assert(!aos_vmm_virtio_console_recreate());
    aos_x86_ioapic_t ioapic;
    assert(aos_x86_ioapic_init(&ioapic,1));
    assert(aos_x86_virtio_init(&ioapic,ram,RAM_BYTES));
    assert(!aos_vmm_virtio_console_init_at(AOS_X86_VIRTIO_BASE+1,16));
    assert(!aos_vmm_virtio_console_init_at(AOS_X86_VIRTIO_BASE,17));
    assert(aos_vmm_virtio_console_init_at(AOS_X86_VIRTIO_BASE,16));
    assert(!aos_vmm_virtio_console_recreate());
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
    /* A transmit larger than the local FIFO must survive quiescence,
     * retaining guest RAM until the final bytes have been copied. */
    enum { OUTPUT_BYTES = AOS_SERIAL_TX_CAPACITY + 2048 };
    unsigned char expected[OUTPUT_BYTES], drained[OUTPUT_BYTES];
    for (unsigned i=0;i<OUTPUT_BYTES;i++) expected[i]=(uint8_t)(i*17u+3u);
    memcpy(ram+0x18000,expected,OUTPUT_BYTES);
    tx[1]=(struct virtq_desc){.addr=0x18000,.len=OUTPUT_BYTES};
    tx_avail->ring[1]=1; tx_avail->idx=2;
    write_reg(REG_VIRTIO_MMIO_QUEUE_NOTIFY,1);
    assert(tx_used->idx==1);
    assert(!aos_vmm_virtio_console_quiesce());
    assert(!aos_vmm_virtio_console_recreate());
    /* Reset cannot discard the retained descriptor during shutdown. */
    write_reg(REG_VIRTIO_MMIO_STATUS,0);
    assert(!aos_vmm_virtio_console_driver_ready());
    assert(!aos_vmm_virtio_console_push_rx_bytes(input,sizeof(input)));
    uint32_t notify=1;
    assert(!aos_x86_virtio_access(AOS_X86_VIRTIO_BASE+REG_VIRTIO_MMIO_QUEUE_NOTIFY,4,true,&notify));
    uint32_t count=aos_vmm_virtio_console_drain_tx(drained,sizeof(drained));
    assert(count==AOS_SERIAL_TX_CAPACITY && tx_used->idx==2);
    assert(aos_vmm_virtio_console_quiesce());
    assert(aos_vmm_virtio_console_quiesce());
    write_reg(REG_VIRTIO_MMIO_INTERRUPT_ACK,1);
    assert(mprotect(ram,RAM_BYTES,PROT_NONE)==0);
    count+=aos_vmm_virtio_console_drain_tx(drained+count,sizeof(drained)-count);
    assert(count==OUTPUT_BYTES && !memcmp(drained,expected,OUTPUT_BYTES));
    assert(!aos_vmm_virtio_console_drain_tx(drained,sizeof(drained)));
    aos_vmm_virtio_console_after_fault();
    assert(!aos_vmm_virtio_console_push_rx_bytes(input,sizeof(input)));
    write_reg(REG_VIRTIO_MMIO_STATUS,0);
    write_reg(REG_VIRTIO_MMIO_STATUS,1);
    write_reg(REG_VIRTIO_MMIO_STATUS,3);
    write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL,1);
    write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES,1);
    write_reg(REG_VIRTIO_MMIO_STATUS,11);
    queue(0,0); queue(1,0x8000);
    write_reg(REG_VIRTIO_MMIO_STATUS,15);
    assert(!aos_vmm_virtio_console_driver_ready());
    assert(!aos_vmm_virtio_console_push_rx_bytes(input,sizeof(input)));
    assert(!aos_x86_virtio_access(AOS_X86_VIRTIO_BASE+REG_VIRTIO_MMIO_QUEUE_NOTIFY,4,true,&notify));
    assert(!ioapic.asserted);
    assert(!aos_vmm_virtio_console_init_at(AOS_X86_VIRTIO_BASE,16));
    assert(munmap(ram,RAM_BYTES)==0);
    aos_x86_virtio_retire();
    for (unsigned generation=0; generation<2; generation++) {
        /* Registration without a new bus fails. Retrying must not revive old
         * bytes or require access to the old, unmapped guest RAM. */
        assert(!aos_vmm_virtio_console_recreate());
        assert(!aos_vmm_virtio_console_driver_ready());
        assert(!aos_vmm_virtio_console_drain_tx(actual,sizeof(actual)));
        ram=mmap(NULL,RAM_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        assert(ram!=MAP_FAILED);
        assert(aos_x86_ioapic_init(&ioapic,1));
        assert(aos_x86_virtio_init(&ioapic,ram,RAM_BYTES));
        assert(aos_vmm_virtio_console_recreate());
        assert(!aos_vmm_virtio_console_recreate());
        assert(read_reg(REG_VIRTIO_MMIO_STATUS)==0);
        assert(read_reg(REG_VIRTIO_MMIO_QUEUE_READY)==0);
        assert(!aos_vmm_virtio_console_tx_active());
        assert(!aos_vmm_virtio_console_drain_tx(actual,sizeof(actual)));
        write_reg(REG_VIRTIO_MMIO_STATUS,1); write_reg(REG_VIRTIO_MMIO_STATUS,3);
        write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL,1);
        write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES,1);
        write_reg(REG_VIRTIO_MMIO_STATUS,11);
        queue(0,0); queue(1,0x8000);
        write_reg(REG_VIRTIO_MMIO_STATUS,15);
        const uint8_t fresh_output[]="replacement output";
        const uint8_t fresh_input[]="replacement input";
        memcpy(ram+0x10000,fresh_output,sizeof(fresh_output));
        tx=(void *)(ram+0x8000);
        tx[0]=(struct virtq_desc){.addr=0x10000,.len=sizeof(fresh_output)};
        tx_avail=(void *)(ram+0xa000);
        tx_avail->ring[0]=0; tx_avail->idx=1;
        write_reg(REG_VIRTIO_MMIO_QUEUE_NOTIFY,1);
        tx_used=(void *)(ram+0xb000);
        assert(tx_used->idx==1 && tx_used->ring[0].id==0);
        assert(aos_vmm_virtio_console_drain_tx(actual,sizeof(actual))==sizeof(fresh_output));
        assert(!memcmp(actual,fresh_output,sizeof(fresh_output)));
        rx=(void *)ram;
        rx[0]=(struct virtq_desc){.addr=0x11000,.len=64,.flags=VIRTQ_DESC_F_WRITE};
        rx_avail=(void *)(ram+0x2000);
        rx_avail->ring[0]=0; rx_avail->idx=1;
        assert(aos_vmm_virtio_console_push_rx_bytes(fresh_input,sizeof(fresh_input)));
        rx_used=(void *)(ram+0x3000);
        assert(rx_used->idx==1 && rx_used->ring[0].len==sizeof(fresh_input));
        assert(!memcmp(ram+0x11000,fresh_input,sizeof(fresh_input)));
        /* Leave old input and output buffered at retirement. Neither may
         * appear in the next generation's descriptor or drain result. */
        assert(aos_vmm_virtio_console_push_rx_bytes(input,sizeof(input)));
        tx_avail->ring[1]=0; tx_avail->idx=2;
        write_reg(REG_VIRTIO_MMIO_QUEUE_NOTIFY,1);
        assert(aos_vmm_virtio_console_quiesce());
        aos_x86_virtio_retire();
        assert(mprotect(ram,RAM_BYTES,PROT_NONE)==0);
        aos_vmm_virtio_console_after_fault();
        assert(!aos_vmm_virtio_console_push_rx_bytes(input,sizeof(input)));
        assert(munmap(ram,RAM_BYTES)==0);
    }
    puts("PASS: console drain safety, fresh TX/RX and isolation across recreated devices");
}
