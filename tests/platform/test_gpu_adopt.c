#define _GNU_SOURCE
#include <platform/vmm_virtio_gpu.h>
#include <platform/gpu_framebuffer.h>
#include <libvmm/virtio/gpu.h>
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>

static bool fail_init, fail_quiesce;
static unsigned registrations, signals;
static aos_gpu_framebuffer_t *current;
static virtio_device_funs_t funs;
void seL4_Signal(seL4_CPtr cap) { (void)cap; ++signals; }
void seL4_Wait(seL4_CPtr cap,seL4_Word *badge) { (void)cap; (void)badge; assert(0); }
void *virtio_gpa_to_hva(uint64_t gpa,size_t bytes) { (void)gpa; (void)bytes; return NULL; }
int virtio_copy_from_gpa(uint64_t gpa,size_t off,void *out,size_t bytes)
{ (void)gpa; (void)off; (void)out; (void)bytes; return -1; }
bool aos_gpu_framebuffer_init(aos_gpu_framebuffer_t *a,virtio_gpu_2d_t *engine)
{
    (void)engine;
    assert(a->region && a->exchange && a->read_gpa && a->validate_gpa);
    assert(!a->scanout_handle && !a->cursor_handle && !a->next_id);
    current=a;
    return true;
}
bool virtio_mmio_gpu_init(virtio_gpu_device_t *g,uintptr_t base,uintptr_t size,size_t irq)
{
    (void)base; (void)size; (void)irq;
    assert(!g->reset_failed && !g->quiescing && !g->device.funs);
    ++registrations;
    g->device.funs=&funs;
    return !fail_init;
}
bool virtio_gpu_quiesce(virtio_gpu_device_t *g)
{
    g->quiescing=true;
    return !fail_quiesce;
}
static void detach(aos_fb_region_t *r)
{
    assert(!aos_vmm_virtio_gpu_detach());
    assert(r->detach.version==AOS_FB_DETACH_VERSION && r->detach.request==1);
    r->detach.ack=1;
    assert(aos_vmm_virtio_gpu_detach());
    assert(!current->region);
}
int main(void)
{
    aos_fb_region_t *old=mmap((void *)AOS_FB_SHMEM_VA,AOS_FB_CLIENT_STRIDE,
        PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(old==(void *)AOS_FB_SHMEM_VA);
    static aos_fb_region_t fresh, failed, final;
    assert(!aos_vmm_virtio_gpu_adopt(0,1,&fresh));
    assert(aos_vmm_virtio_gpu_init());
    assert(!aos_vmm_virtio_gpu_init());
    current->scanout_handle=77;
    current->cursor_handle=78;
    current->next_id=99;
    fail_quiesce=true;
    assert(!aos_vmm_virtio_gpu_detach() && !old->detach.request);
    fail_quiesce=false;
    detach(old);
    assert(!mprotect(old,AOS_FB_CLIENT_STRIDE,PROT_NONE));
    assert(aos_vmm_virtio_gpu_detach());
    assert(!aos_vmm_virtio_gpu_adopt(1,1,&fresh));
    assert(!aos_vmm_virtio_gpu_adopt(0,0,&fresh));
    assert(!aos_vmm_virtio_gpu_adopt(0,1,NULL));
    fresh.detach.request=1;
    assert(!aos_vmm_virtio_gpu_adopt(0,1,&fresh));
    fresh.detach.request=0;
    assert(aos_vmm_virtio_gpu_adopt(0,1,&fresh));
    assert(current->region==&fresh);
    assert(!aos_vmm_virtio_gpu_adopt(0,2,&failed));
    detach(&fresh);
    assert(!aos_vmm_virtio_gpu_adopt(0,1,&failed));
    fail_init=true;
    assert(!aos_vmm_virtio_gpu_adopt(0,2,&failed));
    assert(current->region==&failed);
    detach(&failed);
    assert(!aos_vmm_virtio_gpu_adopt(0,2,&final));
    fail_init=false;
    assert(aos_vmm_virtio_gpu_adopt(0,4,&final));
    assert(registrations==4 && signals>=6);
    detach(&final);
    assert(!munmap(old,AOS_FB_CLIENT_STRIDE));
    puts("PASS: GPU adoption retires old state, rejects stale ownership and cleans failed registration");
}
