#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <platform/blk_rebind.h>
#include <platform/x86_virtio.h>
#include <platform/vmm_virtio_blk.h>
#include <platform/blk_virt_pump.h>
#include <contracts/blk_virt_contract.h>
#include "sel4_ipc.h"
#include "system_desc.h"
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/virtio.h>
#include <libvmm/virtio/virtq.h>
#undef vprintf

const char vmm_pd_name[]="x86-block-test";
static unsigned attachments;
static unsigned detachments, detach_failure;
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
    assert(cap==PD_CNODE_SLOT_BLK_VIRT_EP);
    assert(request->opcode==BLK_VIRT_OP_ATTACH || request->opcode==BLK_VIRT_OP_DETACH);
    assert(request->length==sizeof(blk_virt_attach_req_t));
    blk_virt_attach_req_t attach;
    memcpy(&attach,request->data,sizeof(attach));
    assert(attach.version==BLK_VIRT_CONTRACT_VERSION);
    assert(attach.client_id==0 && attach.vmm_slot==BLK_VIRT_VMM_SLOT_PRIMARY && attach.media_id==0);
    if (request->opcode==BLK_VIRT_OP_ATTACH) attachments++;
    else detachments++;
    if (request->opcode==BLK_VIRT_OP_ATTACH) {
        client.info->capacity=256;
        client.info->sector_size=512;
        client.info->block_size=1;
        client.info->ready=1;
    }
    memset(reply,0,sizeof(*reply));
    reply->length=sizeof(blk_virt_attach_reply_t);
    uint32_t words[]={BLK_VIRT_OK,BLK_VIRT_CONTRACT_VERSION,BLK_VIRT_HW_VIRTIO_BLK};
    memcpy(reply->data,words,sizeof(words));
    if (request->opcode==BLK_VIRT_OP_DETACH) {
        if (detach_failure==1) reply->length=0;
        if (detach_failure==2) reply->data[4]++;
        if (detach_failure==3) reply->data[0]=BLK_VIRT_ERR_BUSY;
    }
}
enum { RAM_BYTES = 0x20000 };
static uint8_t *ram, *region;
static void wr(uintptr_t base, uint32_t offset, uint32_t value)
{ assert(aos_x86_virtio_access(base+offset,4,true,&value)); }
int main(void)
{
    ram=mmap(NULL,RAM_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    region=mmap(NULL,AOS_BLK_SHMEM_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(ram!=MAP_FAILED && region!=MAP_FAILED);
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
    assert(aos_x86_virtio_init(&ioapic,ram,RAM_BYTES));
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
    /* Restore the deliberate rebind-preservation fixture, then drain. */
    client.req->head=client.req->tail=0;
    client.resp->head=client.resp->tail=0;
    for (detach_failure=1; detach_failure<=3; detach_failure++)
        assert(!aos_vmm_virtio_blk_detach());
    detach_failure=0;
    assert(aos_vmm_virtio_blk_detach() && detachments==4);
    assert(aos_vmm_virtio_blk_detach() && detachments==4);
    aos_vmm_virtio_blk_resp_ready();
    aos_vmm_virtio_blk_after_fault();
    assert(!aos_vmm_virtio_blk_init_at(0,base,17,region) && attachments==1);
    aos_x86_virtio_retire();
    assert(mprotect(ram,RAM_BYTES,PROT_NONE)==0);
    assert(mprotect(region,AOS_BLK_SHMEM_SIZE,PROT_NONE)==0);
    for (unsigned generation=1; generation<=3; generation++) {
        uint8_t *fresh=mmap(NULL,AOS_BLK_SHMEM_SIZE,PROT_READ|PROT_WRITE,
            MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        uint8_t *fresh_ram=mmap(NULL,RAM_BYTES,PROT_READ|PROT_WRITE,
            MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        assert(fresh!=MAP_FAILED && fresh_ram!=MAP_FAILED);
        aos_blk_client_bind(fresh,0,&client);
        aos_blk_client_init_queues(&client);
        aos_blk_client_set_ram_disk(&client,boot_media,3u);
        aos_blk_virt_reset(&server);
        assert(aos_blk_virt_add_client(&server,&client)==0);
        blk_virt_rebind_reply_t attachment={BLK_VIRT_OK,BLK_VIRT_REBIND_VERSION,
            generation,generation==3 ? BLK_VIRT_HW_VIRTIO_BLK : BLK_VIRT_HW_NONE};
        assert(!aos_vmm_virtio_blk_adopt(0,fresh,&attachment));
        aos_blk_storage_init(client.info,3u);
        client.req->head=client.req->tail=7;
        client.resp->head=client.resp->tail=9;
        client.data[31]=0xa5;
        assert(!aos_vmm_virtio_blk_adopt(0,fresh,NULL));
        assert(!aos_vmm_virtio_blk_adopt(1,fresh,&attachment));
        assert(!aos_vmm_virtio_blk_adopt(0,fresh+1,&attachment));
        if (generation==1) {
            /* Failed registration still owns the newly rebound backend. */
            assert(!aos_vmm_virtio_blk_adopt(0,fresh,&attachment));
            assert(aos_vmm_virtio_blk_detach() && detachments==5);
        } else {
            assert(aos_x86_ioapic_init(&ioapic,1));
            assert(aos_x86_virtio_init(&ioapic,fresh_ram,RAM_BYTES));
            attachment.generation=generation-1;
            assert(!aos_vmm_virtio_blk_adopt(0,fresh,&attachment));
            attachment.generation=generation;
            assert(aos_vmm_virtio_blk_adopt(0,fresh,&attachment));
            assert(client.req->head==7 && client.req->tail==7 &&
                client.resp->head==9 && client.resp->tail==9 && client.data[31]==0xa5);
            assert(attachments==1 && !aos_vmm_virtio_blk_guest_io_completed());
            assert(!aos_vmm_virtio_blk_adopt(0,fresh,&attachment));
            assert(aos_x86_virtio_access(base+REG_VIRTIO_MMIO_STATUS,4,false,&value) && value==0);
            assert(aos_x86_virtio_access(base+0x100,4,false,&value) && value==24);
            assert(aos_x86_virtio_access(base+0x108,4,false,&value) && value==AOS_BLK_GUEST_MAX_SEGMENT_SIZE);
            bool boot_read=aos_vmm_virtio_blk_read_boot(1,1,boot_copy,sizeof(boot_copy),boot_wait);
            assert(boot_read==(attachment.hw_state==BLK_VIRT_HW_VIRTIO_BLK));
            if (boot_read)
                assert(!memcmp(boot_copy,boot_media+AOS_BLK_TRANSFER_SIZE,sizeof(boot_copy)));
            wr(base,REG_VIRTIO_MMIO_STATUS,1); wr(base,REG_VIRTIO_MMIO_STATUS,3);
            wr(base,REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL,1);
            wr(base,REG_VIRTIO_MMIO_DRIVER_FEATURES,1);
            wr(base,REG_VIRTIO_MMIO_STATUS,11);
            wr(base,REG_VIRTIO_MMIO_QUEUE_SEL,0);
            wr(base,REG_VIRTIO_MMIO_QUEUE_NUM,8);
            wr(base,REG_VIRTIO_MMIO_QUEUE_DESC_LOW,0);
            wr(base,REG_VIRTIO_MMIO_QUEUE_AVAIL_LOW,0x2000);
            wr(base,REG_VIRTIO_MMIO_QUEUE_USED_LOW,0x3000);
            wr(base,REG_VIRTIO_MMIO_QUEUE_READY,1);
            wr(base,REG_VIRTIO_MMIO_STATUS,15);
            struct virtq_desc *desc=(void *)fresh_ram;
            desc[0]=(struct virtq_desc){.addr=0x4000,.len=16,.flags=VIRTQ_DESC_F_NEXT,.next=1};
            desc[1]=(struct virtq_desc){.addr=0x5000,.len=AOS_BLK_TRANSFER_SIZE,
                .flags=VIRTQ_DESC_F_NEXT|VIRTQ_DESC_F_WRITE,.next=2};
            desc[2]=(struct virtq_desc){.addr=0x7000,.len=1,.flags=VIRTQ_DESC_F_WRITE};
            /* Read sector 8 (the second 4 KiB backend block). */
            fresh_ram[0x4008]=8;
            fresh_ram[0x7000]=0xff;
            struct virtq_avail *avail=(void *)(fresh_ram+0x2000);
            avail->ring[0]=0; avail->idx=1;
            wr(base,REG_VIRTIO_MMIO_QUEUE_NOTIFY,0);
            aos_vmm_virtio_blk_after_fault();
            assert(aos_blk_virt_pump(&server)==1);
            aos_vmm_virtio_blk_resp_ready();
            struct virtq_used *used=(void *)(fresh_ram+0x3000);
            assert(used->idx==1 && used->ring[0].id==0);
            assert(fresh_ram[0x7000]==0);
            assert(!memcmp(fresh_ram+0x5000,boot_media+AOS_BLK_TRANSFER_SIZE,AOS_BLK_TRANSFER_SIZE));
            assert(aos_vmm_virtio_blk_detach());
            aos_x86_virtio_retire();
        }
        unsigned old_kicks=kicks;
        assert(mprotect(fresh,AOS_BLK_SHMEM_SIZE,PROT_NONE)==0);
        assert(mprotect(fresh_ram,RAM_BYTES,PROT_NONE)==0);
        aos_vmm_virtio_blk_resp_ready();
        aos_vmm_virtio_blk_after_fault();
        assert(kicks==old_kicks);
        assert(munmap(fresh,AOS_BLK_SHMEM_SIZE)==0);
        assert(munmap(fresh_ram,RAM_BYTES)==0);
    }
    assert(munmap(ram,RAM_BYTES)==0);
    assert(munmap(region,AOS_BLK_SHMEM_SIZE)==0);
    puts("PASS: block rebind adoption, fresh descriptor reads and failed-registration cleanup");
}
