#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <platform/x86_virtio.h>
#include <platform/vmm_virtio_net.h>
#include <platform/net_virt_pump.h>
#include <contracts/net_virt_contract.h>
#include "sel4_ipc.h"
#include "system_desc.h"
#include <libvmm/virtio/virtio.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/virtq.h>
#undef vprintf

const char vmm_pd_name[] = "x86-net-test";
static unsigned attachments, kicks;
static bool reject_attach = true;
static const uintptr_t base = AOS_X86_VIRTIO_BASE + 2u*AOS_X86_VIRTIO_STRIDE;
static _Alignas(4096) unsigned char ram[0x20000];
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
    assert(cap==PD_CNODE_SLOT_NET_VIRT_EP && request->opcode==NET_VIRT_OP_ATTACH);
    assert(request->length==sizeof(net_virt_attach_req_t));
    net_virt_attach_req_t attach;
    memcpy(&attach,request->data,sizeof(attach));
    assert(attach.version==NET_VIRT_CONTRACT_VERSION && attach.client_id==0 &&
           attach.vmm_slot==NET_VIRT_VMM_SLOT_PRIMARY);
    attachments++;
    net_virt_attach_reply_t result={.status=reject_attach ? NET_VIRT_ERR_UNAVAILABLE : NET_VIRT_OK,
        .version=NET_VIRT_CONTRACT_VERSION,.hw_state=NET_VIRT_HW_NONE};
    memset(reply,0,sizeof(*reply));
    reply->length=sizeof(result);
    memcpy(reply->data,&result,sizeof(result));
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
int main(void)
{
    aos_x86_ioapic_t ioapic;
    assert(aos_x86_ioapic_init(&ioapic,1));
    assert(aos_x86_virtio_init(&ioapic,ram,sizeof(ram)));
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
    assert(read_reg(REG_VIRTIO_MMIO_DEVICE_ID)==VIRTIO_DEVICE_ID_NET);
    assert(read_reg(0x100)==2 && (read_reg(0x104)&0xffff)==0x100);
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
    write_reg(REG_VIRTIO_MMIO_INTERRUPT_ACK,1);
    assert(!ioapic.asserted);
    puts("PASS: network adapter placement and exact TX/RX through x86 MMIO, canonical queues and IOAPIC");
}
