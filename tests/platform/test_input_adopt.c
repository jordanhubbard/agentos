/* SPDX-License-Identifier: BSD-2-Clause */
#include <platform/vmm_virtio_input.h>
#include <libvmm/virtio/input.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static bool fail_pointer;
static unsigned registrations, delivered, signals;
static virtio_device_funs_t funs;
void seL4_Signal(seL4_CPtr cap) { (void)cap; ++signals; }
bool virtio_mmio_input_init(virtio_input_device_t *d, enum virtio_input_kind kind,
    virtio_input_receive_fn receive, void *context, uintptr_t base,
    uintptr_t size, size_t irq)
{
    (void)base; (void)size; (void)irq;
    ++registrations;
    d->device.funs=&funs;
    d->receive=receive;
    d->context=context;
    d->quiesced=false;
    return !(fail_pointer && kind==VIRTIO_INPUT_POINTER);
}
void virtio_input_quiesce(virtio_input_device_t *d) { d->quiesced=true; }
bool virtio_input_drain(virtio_input_device_t *d)
{
    uint8_t event[8];
    if (d->quiesced || !d->receive(d->context,event)) return false;
    assert(event[0]==1 && event[2]==30 && event[4]==1);
    ++delivered;
    return true;
}
static void detach(aos_input_client_region_t *r)
{
    assert(!aos_vmm_virtio_input_detach());
    assert(r->detach.version==AOS_INPUT_DETACH_VERSION && r->detach.request==1);
    r->detach.ack=1;
    assert(aos_vmm_virtio_input_detach());
}
int main(void)
{
    aos_input_client_region_t *old=mmap((void *)AOS_INPUT_SHMEM_VA,
        AOS_INPUT_FRAME_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(old==(void *)AOS_INPUT_SHMEM_VA);
    static aos_input_client_region_t fresh, failed, final;
    assert(!aos_vmm_virtio_input_adopt(0,1,&fresh));
    assert(aos_vmm_virtio_input_init());
    assert(!aos_vmm_virtio_input_init());
    detach(old);
    assert(!mprotect(old,AOS_INPUT_FRAME_SIZE,PROT_NONE));
    aos_vmm_virtio_input_drain();
    assert(aos_vmm_virtio_input_detach());
    assert(!aos_vmm_virtio_input_adopt(1,1,&fresh));
    assert(!aos_vmm_virtio_input_adopt(0,0,&fresh));
    assert(!aos_vmm_virtio_input_adopt(0,1,NULL));
    fresh.detach.request=1;
    assert(!aos_vmm_virtio_input_adopt(0,1,&fresh));
    fresh.detach.request=0;
    fresh.devices[0].events[0]=(aos_input_event_t){1,30,1};
    fresh.devices[0].tail=1;
    assert(aos_vmm_virtio_input_adopt(0,1,&fresh));
    assert(fresh.devices[0].tail==1 && fresh.devices[0].head==0);
    aos_vmm_virtio_input_drain();
    assert(delivered==1 && fresh.devices[0].head==1);
    assert(!aos_vmm_virtio_input_adopt(0,2,&failed));
    detach(&fresh);
    assert(!aos_vmm_virtio_input_adopt(0,1,&failed));
    fail_pointer=true;
    assert(!aos_vmm_virtio_input_adopt(0,2,&failed));
    aos_vmm_virtio_input_drain();
    assert(delivered==1);
    detach(&failed);
    assert(!aos_vmm_virtio_input_adopt(0,2,&final));
    fail_pointer=false;
    assert(aos_vmm_virtio_input_adopt(0,3,&final));
    assert(registrations==8 && signals>=7);
    detach(&final);
    assert(!munmap(old,AOS_INPUT_FRAME_SIZE));
    puts("PASS: input adoption preserves events, rejects stale ownership and cleans partial registration");
}
