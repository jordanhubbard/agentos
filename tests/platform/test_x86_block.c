#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <platform/x86_virtio.h>
#include <platform/vmm_virtio_blk.h>
#include <platform/blk_virt_pump.h>
#include <contracts/blk_virt_contract.h>
#include "sel4_ipc.h"
#include "system_desc.h"
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/virtio.h>
#undef vprintf

const char vmm_pd_name[]="x86-block-test";
static unsigned attachments;
static aos_blk_virt_client_t client;
static aos_blk_virt_t server;
static unsigned kicks, waits;
static uint8_t boot_media[3u * AOS_BLK_TRANSFER_SIZE];
static uint8_t boot_copy[AOS_BLK_TRANSFER_SIZE];
int printf_(const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt); int n=vprintf(fmt,ap); va_end(ap); return n;
}
void seL4_Signal(seL4_CPtr cap) { assert(cap == PD_CNODE_SLOT_BLK_VIRT_NOTIFY); kicks++; }
static void boot_wait(void)
{
    waits++;
    assert(aos_blk_virt_pump(&server) == 1u);
}
void sel4_call(seL4_CPtr cap, const sel4_msg_t *request, sel4_msg_t *reply)
{
    assert(cap==PD_CNODE_SLOT_BLK_VIRT_EP && request->opcode==BLK_VIRT_OP_ATTACH);
    assert(request->length==sizeof(blk_virt_attach_req_t));
    blk_virt_attach_req_t attach;
    memcpy(&attach,request->data,sizeof(attach));
    assert(attach.version==BLK_VIRT_CONTRACT_VERSION);
    assert(attach.client_id==0 && attach.vmm_slot==BLK_VIRT_VMM_SLOT_PRIMARY && attach.media_id==0);
    attachments++;
    client.info->capacity=256;
    client.info->sector_size=512;
    client.info->block_size=1;
    client.info->ready=1;
    memset(reply,0,sizeof(*reply));
    reply->length=sizeof(blk_virt_attach_reply_t);
    uint32_t words[]={BLK_VIRT_OK,BLK_VIRT_CONTRACT_VERSION,BLK_VIRT_HW_VIRTIO_BLK};
    memcpy(reply->data,words,sizeof(words));
}
static _Alignas(4096) uint8_t ram[0x20000];
static _Alignas(4096) uint8_t region[AOS_BLK_SHMEM_SIZE];
int main(void)
{
    uintptr_t base=AOS_X86_VIRTIO_BASE+AOS_X86_VIRTIO_STRIDE;
    assert(!aos_vmm_virtio_blk_init_at(0,0,17,region));
    assert(!aos_vmm_virtio_blk_init_at(0,base+1,17,region));
    assert(!aos_vmm_virtio_blk_init_at(0,base,17,NULL));
    assert(!aos_vmm_virtio_blk_init_at(0,base,17,region+1));
    assert(!aos_vmm_virtio_blk_init_at(AOS_HOST_BLK_MEDIA_COUNT,base,17,region));
    assert(!attachments);
    aos_blk_client_bind(region,0,&client);
    aos_x86_ioapic_t ioapic;
    assert(aos_x86_ioapic_init(&ioapic,1));
    assert(aos_x86_virtio_init(&ioapic,ram,sizeof(ram)));
    assert(aos_vmm_virtio_blk_init_at(0,base,17,region) && attachments==1);
    assert(!aos_vmm_virtio_blk_guest_io_completed());
    uint32_t value=0;
    assert(aos_x86_virtio_access(base+REG_VIRTIO_MMIO_DEVICE_ID,4,false,&value) && value==2);
    assert(aos_x86_virtio_access(base+0x100,4,false,&value) && value==2048);
    for (unsigned i = 0; i < sizeof(boot_media); i++) boot_media[i] = (uint8_t)(i / 4096u + i);
    aos_blk_client_set_ram_disk(&client, boot_media, 3u);
    aos_blk_virt_reset(&server);
    assert(aos_blk_virt_add_client(&server, &client) == 0);
    assert(!aos_vmm_virtio_blk_read_boot(0, 0, boot_copy, sizeof(boot_copy), boot_wait));
    assert(!aos_vmm_virtio_blk_read_boot(0, 1, NULL, sizeof(boot_copy), boot_wait));
    assert(!aos_vmm_virtio_blk_read_boot(0, 1, boot_copy, sizeof(boot_copy) - 1u, boot_wait));
    assert(!aos_vmm_virtio_blk_read_boot(UINT64_MAX, 1, boot_copy, sizeof(boot_copy), boot_wait));
    assert(!aos_vmm_virtio_blk_read_boot(0, 1, boot_copy, sizeof(boot_copy), NULL));
    assert(!kicks && !waits);
    assert(aos_vmm_virtio_blk_read_boot(1, 1, boot_copy, sizeof(boot_copy), boot_wait));
    assert(kicks == 1u && waits == 1u);
    assert(!memcmp(boot_copy, boot_media + AOS_BLK_TRANSFER_SIZE, sizeof(boot_copy)));
    memset(boot_copy, 0x5a, sizeof(boot_copy));
    assert(!aos_vmm_virtio_blk_read_boot(7, 1, boot_copy, sizeof(boot_copy), boot_wait));
    for (unsigned i = 0; i < sizeof(boot_copy); i++) assert(boot_copy[i] == 0x5a);
    value = VIRTIO_CONFIG_S_DRIVER_OK;
    assert(aos_x86_virtio_access(base + REG_VIRTIO_MMIO_STATUS, 4, true, &value));
    assert(!aos_vmm_virtio_blk_read_boot(0, 1, boot_copy, sizeof(boot_copy), boot_wait));
    assert(kicks == 2u && waits == 2u);
    assert(!aos_vmm_virtio_blk_guest_io_completed());
    client.signal->req_consumer_signalled=0x55;
    client.req->head=3; client.req->tail=4;
    client.resp->head=5; client.resp->tail=6;
    client.data[0]=0xa5;
    assert(!aos_vmm_virtio_blk_init_at(0,base,17,region));
    assert(attachments==1 && client.signal->req_consumer_signalled==0x55 && client.data[0]==0xa5);
    aos_vmm_virtio_blk_init(0);
    assert(attachments==1 && client.signal->req_consumer_signalled==0x55);
    assert(client.req->head==3 && client.req->tail==4 && client.resp->head==5 && client.resp->tail==6);
    puts("PASS: block placement, real MMIO capacity, boot queue read and rebind preservation");
}
