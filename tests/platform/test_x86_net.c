#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <platform/x86_virtio.h>
#include <platform/vmm_virtio_net.h>
#include <platform/net_virt_pump.h>
#include <platform/net_rebind.h>
#include <contracts/net_virt_contract.h>
#include "sel4_ipc.h"
#include "system_desc.h"
#include <libvmm/virtio/virtio.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/virtq.h>
#undef vprintf

const char vmm_pd_name[] = "x86-net-test";
static unsigned attachments, kicks;
static unsigned detachments, detach_failure;
static bool reject_attach = true;
static bool host_fixture;
static const uintptr_t base = AOS_X86_VIRTIO_BASE + 2u*AOS_X86_VIRTIO_STRIDE;
static unsigned char *ram;
enum { RAM_BYTES = 0x20000 };
static _Alignas(4096) unsigned char region[AOS_NET_SHMEM_SIZE];
int printf_(const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt); int n=vprintf(fmt,ap); va_end(ap); return n;
}
void seL4_Signal(seL4_CPtr cap)
{
    assert(cap==PD_CNODE_SLOT_NET_VIRT_NOTIFY);
    kicks++;
}
void sel4_call(seL4_CPtr cap, const sel4_msg_t *request, sel4_msg_t *reply)
{
    assert(cap==PD_CNODE_SLOT_NET_VIRT_EP);
    assert(request->opcode==NET_VIRT_OP_ATTACH || request->opcode==NET_VIRT_OP_DETACH);
    assert(request->length==sizeof(net_virt_attach_req_t));
    net_virt_attach_req_t attach;
    memcpy(&attach,request->data,sizeof(attach));
    assert(attach.version==NET_VIRT_CONTRACT_VERSION && attach.client_id==0 &&
           attach.vmm_slot==NET_VIRT_VMM_SLOT_PRIMARY);
    if (request->opcode==NET_VIRT_OP_ATTACH) attachments++;
    else detachments++;
    net_virt_attach_reply_t result={.status=reject_attach ? NET_VIRT_ERR_UNAVAILABLE : NET_VIRT_OK,
        .version=NET_VIRT_CONTRACT_VERSION,
        .hw_state=host_fixture ? NET_VIRT_HW_NET_PD : NET_VIRT_HW_NONE};
    memset(reply,0,sizeof(*reply));
    reply->length=sizeof(result);
    memcpy(reply->data,&result,sizeof(result));
    const uint8_t assigned_mac[6] = {0x52,0x54,0,0x12,0x34,0x56};
    memcpy(reply->data+12,assigned_mac,sizeof(assigned_mac));
    if (request->opcode==NET_VIRT_OP_DETACH) {
        if (detach_failure==1) reply->length=0;
        if (detach_failure==2) reply->data[4]++;
        if (detach_failure==3) reply->data[0]=NET_VIRT_ERR_UNAVAILABLE;
    }
}
static void write_reg(unsigned offset, uint32_t value)
{ assert(aos_x86_virtio_access(base+offset,4,true,&value)); }
static uint32_t read_reg(unsigned offset)
{
    uint32_t value=0; assert(aos_x86_virtio_access(base+offset,4,false,&value)); return value;
}
static void queue(unsigned index, unsigned address)
{
    write_reg(REG_VIRTIO_MMIO_QUEUE_SEL,index);
    write_reg(REG_VIRTIO_MMIO_QUEUE_NUM,QUEUE_SIZE);
    write_reg(REG_VIRTIO_MMIO_QUEUE_DESC_LOW,address);
    write_reg(REG_VIRTIO_MMIO_QUEUE_AVAIL_LOW,address+0x2000);
    write_reg(REG_VIRTIO_MMIO_QUEUE_USED_LOW,address+0x3000);
    write_reg(REG_VIRTIO_MMIO_QUEUE_READY,1);
}
int main(int argc, char **argv)
{
    assert(argc==1 || (argc==2 && !strcmp(argv[1],"host-fixture")));
    host_fixture=argc==2;
    ram=mmap(NULL,RAM_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(ram!=MAP_FAILED);
    aos_x86_ioapic_t ioapic;
    assert(aos_x86_ioapic_init(&ioapic,1));
    assert(aos_x86_virtio_init(&ioapic,ram,RAM_BYTES));
    aos_vmm_virtio_net_quiesce(); /* An absent device remains initializable. */
    assert(!aos_vmm_virtio_net_init_at(0,0,18,region));
    assert(!aos_vmm_virtio_net_init_at(0,base+1,18,region));
    assert(!aos_vmm_virtio_net_init_at(0,base,18,NULL));
    assert(!aos_vmm_virtio_net_init_at(0,base,18,region+1));
    assert(!aos_vmm_virtio_net_init_at(AOS_NET_GUEST_CLIENTS,base,18,region));
    assert(!attachments);
    assert(!aos_vmm_virtio_net_init_at(0,base,18,region) && attachments==1);
    assert(!aos_x86_virtio_contains(base));
    reject_attach=false;
    assert(aos_vmm_virtio_net_init_at(0,base,18,region) && attachments==2);
    assert(aos_vmm_virtio_net_host_ready()==host_fixture);
    assert(!aos_vmm_virtio_net_guest_io_completed());
    assert(read_reg(REG_VIRTIO_MMIO_DEVICE_ID)==VIRTIO_DEVICE_ID_NET);
    assert(read_reg(0x100)==(host_fixture ? 0x12005452u : 2u));
    assert((read_reg(0x104)&0xffff)==(host_fixture ? 0x5634u : 0x100u));
    write_reg(REG_VIRTIO_MMIO_STATUS,1); write_reg(REG_VIRTIO_MMIO_STATUS,3);
    write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL,0);
    write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES,(1u<<5)|(1u<<15));
    write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL,1);
    write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES,1);
    write_reg(REG_VIRTIO_MMIO_STATUS,11);
    queue(0,0); queue(1,0x8000);
    write_reg(REG_VIRTIO_MMIO_STATUS,15);

    unsigned char packet[64];
    for (unsigned i=0; i<sizeof(packet); i++) packet[i]=(uint8_t)(i+1u);
    memcpy(ram+0x10000,packet,sizeof(packet));
    struct virtq_desc *tx=(void *)(ram+0x8000);
    tx[0]=(struct virtq_desc){.addr=0x12000,.len=12,.flags=VIRTQ_DESC_F_NEXT,.next=1};
    tx[1]=(struct virtq_desc){.addr=0x10000,.len=sizeof(packet)};
    struct virtq_avail *tx_avail=(void *)(ram+0xa000);
    tx_avail->ring[0]=0; tx_avail->idx=1;
    write_reg(REG_VIRTIO_MMIO_QUEUE_NOTIFY,1);
    aos_vmm_virtio_net_after_fault();
    assert(kicks && ioapic.asserted==(1u<<18));
    struct virtq_used *tx_used=(void *)(ram+0xb000);
    assert(tx_used->idx==1 && tx_used->ring[0].id==0 && tx_used->ring[0].len==sizeof(packet));
    aos_net_virt_client_t client;
    aos_net_client_bind(region,0,&client);
    assert(aos_net_queue_length(client.tx_active)==1);
    assert(!aos_vmm_virtio_net_init_at(0,base,18,region) && attachments==2);
    assert(aos_net_queue_length(client.tx_active)==1);
    assert(!memcmp(client.tx_data,packet,sizeof(packet)));
    write_reg(REG_VIRTIO_MMIO_INTERRUPT_ACK,1);
    assert(!ioapic.asserted);

    struct virtq_desc *rx=(void *)ram;
    rx[0]=(struct virtq_desc){.addr=0x11000,.len=128,.flags=VIRTQ_DESC_F_WRITE};
    struct virtq_avail *rx_avail=(void *)(ram+0x2000);
    rx_avail->ring[0]=0; rx_avail->idx=1;
    aos_net_virt_t server;
    aos_net_virt_reset(&server);
    assert(aos_net_virt_add_client(&server,&client)==0);
    assert(aos_net_virt_pump(&server)==1);
    aos_vmm_virtio_net_rx_ready();
    struct virtq_used *rx_used=(void *)(ram+0x3000);
    assert(rx_used->idx==1 && rx_used->ring[0].id==0 && rx_used->ring[0].len==12+sizeof(packet));
    assert(ram[0x1100a]==1 && ram[0x1100b]==0);
    assert(!memcmp(ram+0x1100c,packet,sizeof(packet)));
    assert(ioapic.asserted==(1u<<18));
    assert(!aos_net_queue_length(client.rx_active));
    assert(aos_vmm_virtio_net_guest_io_completed()==host_fixture);
    write_reg(REG_VIRTIO_MMIO_INTERRUPT_ACK,1);
    assert(!ioapic.asserted);
    /* Leave a second packet waiting on the canonical RX queue when guest
     * execution stops. After quiescence, even mapped-but-inaccessible guest
     * RAM must not be touched by late RX notifications or guest queue kicks. */
    tx_avail->ring[1]=0; tx_avail->idx=2;
    write_reg(REG_VIRTIO_MMIO_QUEUE_NOTIFY,1);
    aos_vmm_virtio_net_after_fault();
    assert(aos_net_virt_pump(&server)==1);
    assert(aos_net_queue_length(client.rx_active)==1);
    tx_avail->ring[2]=0; tx_avail->idx=3;
    write_reg(REG_VIRTIO_MMIO_QUEUE_NOTIFY,1);
    assert(aos_net_queue_length(client.tx_active)==1);
    write_reg(REG_VIRTIO_MMIO_INTERRUPT_ACK,1);
    aos_vmm_virtio_net_quiesce();
    aos_vmm_virtio_net_quiesce();
    assert(!aos_vmm_virtio_net_host_ready());
    assert(!aos_vmm_virtio_net_guest_io_completed());
    unsigned old_kicks=kicks;
    assert(mprotect(ram,RAM_BYTES,PROT_NONE)==0);
    /* Accepted TX owns a packet copy, so its service completion does not
     * depend on the now inaccessible guest pages. */
    assert(aos_net_virt_pump(&server)==1);
    aos_vmm_virtio_net_rx_ready();
    aos_vmm_virtio_net_after_fault();
    uint32_t notify=1;
    assert(!aos_x86_virtio_access(base+REG_VIRTIO_MMIO_QUEUE_NOTIFY,4,true,&notify));
    write_reg(REG_VIRTIO_MMIO_STATUS,0);
    write_reg(REG_VIRTIO_MMIO_STATUS,1);
    write_reg(REG_VIRTIO_MMIO_STATUS,3);
    write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL,0);
    write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES,(1u<<5)|(1u<<15));
    write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL,1);
    write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES,1);
    write_reg(REG_VIRTIO_MMIO_STATUS,11);
    queue(0,0); queue(1,0x8000);
    write_reg(REG_VIRTIO_MMIO_STATUS,15);
    assert(!aos_x86_virtio_access(base+REG_VIRTIO_MMIO_QUEUE_NOTIFY,4,true,&notify));
    aos_vmm_virtio_net_rx_ready();
    assert(kicks==old_kicks && !ioapic.asserted);
    assert(aos_net_queue_length(client.rx_active)==2);
    assert(aos_net_queue_length(client.tx_active)==0);
    assert(!aos_vmm_virtio_net_init_at(0,base,18,region) && attachments==2);
    assert(munmap(ram,RAM_BYTES)==0);
    for (detach_failure=1; detach_failure<=3; detach_failure++)
        assert(!aos_vmm_virtio_net_detach());
    detach_failure=0;
    assert(aos_vmm_virtio_net_detach() && detachments==4);
    assert(aos_vmm_virtio_net_detach() && detachments==4);
    assert(aos_vmm_virtio_net_diagnostic()==0);
    aos_vmm_virtio_net_rx_ready();
    aos_vmm_virtio_net_after_fault();
    assert(kicks==old_kicks);
    aos_x86_virtio_retire();
    assert(mprotect(region,sizeof(region),PROT_NONE)==0);
    unsigned char *fresh=mmap(NULL,AOS_NET_SHMEM_SIZE,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(fresh!=MAP_FAILED);
    aos_net_client_bind(fresh,0,&client);
    aos_net_client_init_buffers(&client);
    net_virt_rebind_reply_t attachment={.status=NET_VIRT_OK,
        .version=NET_VIRT_REBIND_VERSION,.generation=1,
        .hw_state=host_fixture ? NET_VIRT_HW_NET_PD : NET_VIRT_HW_NONE,
        .mac={0x52,0x54,0,0x98,0x76,0x54}};
    assert(!aos_vmm_virtio_net_adopt(0,fresh,NULL));
    assert(!aos_vmm_virtio_net_adopt(1,fresh,&attachment));
    assert(!aos_vmm_virtio_net_adopt(0,fresh+1,&attachment));
    attachment.generation=0;
    assert(!aos_vmm_virtio_net_adopt(0,fresh,&attachment));
    attachment.generation=1;
    /* No fresh bus: registration fails, but DETACH must still release the
     * newly adopted backend. No second ATTACH is sent and old queues are gone. */
    assert(!aos_vmm_virtio_net_adopt(0,fresh,&attachment));
    assert(!aos_vmm_virtio_net_host_ready() && attachments==2);
    assert(aos_vmm_virtio_net_detach() && detachments==5);
    assert(munmap(fresh,AOS_NET_SHMEM_SIZE)==0);

    /* Service generation 2 could have been consumed by a failed local frame
     * mapping before adoption. Newer generations remain recoverable. */
    for (unsigned generation=3; generation<=4; generation++) {
        ram=mmap(NULL,RAM_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        fresh=mmap(NULL,AOS_NET_SHMEM_SIZE,PROT_READ|PROT_WRITE,
            MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        assert(ram!=MAP_FAILED && fresh!=MAP_FAILED);
        assert(aos_x86_ioapic_init(&ioapic,1));
        assert(aos_x86_virtio_init(&ioapic,ram,RAM_BYTES));
        aos_net_client_bind(fresh,0,&client);
        aos_net_client_init_buffers(&client);
        /* A real service may deliver RX between REBIND and adoption. */
        memcpy(client.rx_data,packet,sizeof(packet));
        client.rx_active->buffers[0]=(aos_net_buff_desc_t){.len=sizeof(packet)};
        client.rx_active->tail=1;
        client.rx_free->head=1;
        attachment.generation=generation==3 ? 1u : 3u;
        assert(!aos_vmm_virtio_net_adopt(0,fresh,&attachment));
        attachment.generation=generation;
        assert(aos_vmm_virtio_net_adopt(0,fresh,&attachment));
        assert(attachments==2 && !aos_vmm_virtio_net_guest_io_completed());
        assert(client.rx_active->tail==1 && client.rx_free->head==1);
        assert(!memcmp(client.rx_data,packet,sizeof(packet)));
        assert(read_reg(REG_VIRTIO_MMIO_STATUS)==0);
        assert(read_reg(REG_VIRTIO_MMIO_QUEUE_READY)==0);
        assert(read_reg(0x100)==(host_fixture ? 0x98005452u : 2u));
        assert((read_reg(0x104)&0xffff)==(host_fixture ? 0x5476u : 0x100u));
        assert(!aos_vmm_virtio_net_adopt(0,fresh,&attachment));
        write_reg(REG_VIRTIO_MMIO_STATUS,1); write_reg(REG_VIRTIO_MMIO_STATUS,3);
        write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL,0);
        write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES,(1u<<5)|(1u<<15));
        write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL,1);
        write_reg(REG_VIRTIO_MMIO_DRIVER_FEATURES,1);
        write_reg(REG_VIRTIO_MMIO_STATUS,11);
        queue(0,0); queue(1,0x8000);
        write_reg(REG_VIRTIO_MMIO_STATUS,15);
        rx=(void *)ram;
        rx[0]=(struct virtq_desc){.addr=0x11000,.len=128,.flags=VIRTQ_DESC_F_WRITE};
        rx_avail=(void *)(ram+0x2000);
        rx_avail->ring[0]=0; rx_avail->idx=1;
        aos_vmm_virtio_net_rx_ready();
        rx_used=(void *)(ram+0x3000);
        assert(rx_used->idx==1 && rx_used->ring[0].len==12+sizeof(packet));
        assert(!memcmp(ram+0x1100c,packet,sizeof(packet)));
        memcpy(ram+0x10000,packet,sizeof(packet));
        tx=(void *)(ram+0x8000);
        tx[0]=(struct virtq_desc){.addr=0x12000,.len=12,.flags=VIRTQ_DESC_F_NEXT,.next=1};
        tx[1]=(struct virtq_desc){.addr=0x10000,.len=sizeof(packet)};
        tx_avail=(void *)(ram+0xa000);
        tx_avail->ring[0]=0; tx_avail->idx=1;
        write_reg(REG_VIRTIO_MMIO_QUEUE_NOTIFY,1);
        aos_vmm_virtio_net_after_fault();
        tx_used=(void *)(ram+0xb000);
        assert(tx_used->idx==1 && tx_used->ring[0].len==sizeof(packet));
        assert(aos_net_queue_length(client.tx_active)==1);
        assert(!memcmp(client.tx_data,packet,sizeof(packet)));
        assert(aos_vmm_virtio_net_detach());
        aos_x86_virtio_retire();
        old_kicks=kicks;
        assert(mprotect(ram,RAM_BYTES,PROT_NONE)==0);
        assert(mprotect(fresh,AOS_NET_SHMEM_SIZE,PROT_NONE)==0);
        aos_vmm_virtio_net_rx_ready();
        aos_vmm_virtio_net_after_fault();
        assert(kicks==old_kicks);
        assert(munmap(ram,RAM_BYTES)==0);
        assert(munmap(fresh,AOS_NET_SHMEM_SIZE)==0);
    }
    assert(mprotect(region,sizeof(region),PROT_READ|PROT_WRITE)==0);
    puts("PASS: network TX/RX, retirement, failed adoption cleanup and fresh device generations");
}
