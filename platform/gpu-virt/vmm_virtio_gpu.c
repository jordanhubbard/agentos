/* SPDX-License-Identifier: BSD-2-Clause */
#include <platform/vmm_virtio_gpu.h>
#include <platform/gpu_framebuffer.h>
#include <libvmm/virtio/gpu.h>
#include <libvmm/virtio/gpa.h>
#include "system_desc.h"
#include <sel4/sel4.h>

static virtio_gpu_device_t gpu;
static aos_gpu_framebuffer_t framebuffer;
static bool attempted;
static bool detached;
static uint32_t generation;
#ifdef AGENTOS_GUEST_SECONDARY
static const unsigned client=1;
#else
static const unsigned client=0;
#endif
static aos_fb_region_t *region;

static bool exchange(void *context, const aos_fb_request_t *q, aos_fb_response_t *p)
{
    (void)context;
    const bool report = q->id <= 8 || (q->id & 127u) == 0;
    if (report)
        LOG_VMM("framebuffer exchange: submit id=%u op=%u row=%u\n", q->id, q->operation, q->y);
    if (aos_fb_submit(framebuffer.region,q) != 0) return false;
    seL4_Signal(PD_CNODE_SLOT_FB_PEER_NOTIFY);
    while (aos_fb_receive(framebuffer.region,p) != 0) {
        seL4_Word badge;
        seL4_Wait(PD_CNODE_SLOT_FB_WAIT,&badge);
    }
    /* Freeing a response slot must also wake a backpressured service. */
    seL4_Signal(PD_CNODE_SLOT_FB_PEER_NOTIFY);
    if (report)
        LOG_VMM("framebuffer exchange: response id=%u status=%u\n", p->id, p->status);
    return p->id == q->id;
}
static bool validate(void *context, uint64_t gpa, uint32_t length)
{
    (void)context;
    return gpa <= UINT64_MAX-length && virtio_gpa_to_hva(gpa,length) != NULL;
}
static bool read_guest(void *context, uint64_t gpa, void *out, uint32_t length)
{
    (void)context;
    return virtio_copy_from_gpa(gpa,0,out,length) == 0;
}
static bool initialize(void)
{
    attempted=true;
    framebuffer=(aos_gpu_framebuffer_t){
        .region=region,
        .exchange=exchange, .validate_gpa=validate, .read_gpa=read_guest
    };
    if (!aos_gpu_framebuffer_init(&framebuffer,&gpu.engine) ||
        !virtio_mmio_gpu_init(&gpu,AOS_VIRTIO_GPU_GUEST_IPA,AOS_VIRTIO_GPU_MMIO_SIZE,AOS_VIRTIO_GPU_VIRQ))
        return false;
    LOG_VMM("emulated virtio-gpu IPA 0x%lx IRQ %u (framebuffer client %u)\n",
        AOS_VIRTIO_GPU_GUEST_IPA,AOS_VIRTIO_GPU_VIRQ,client);
    return true;
}
bool aos_vmm_virtio_gpu_init(void)
{
    if (attempted || detached) return false;
    region=(void *)(AOS_FB_SHMEM_VA+client*AOS_FB_CLIENT_STRIDE);
    return initialize();
}
bool aos_vmm_virtio_gpu_adopt(uint32_t owner,uint32_t next,aos_fb_region_t *fresh)
{
    if (!detached || owner!=client || !next || next<=generation || !fresh ||
        fresh->detach.version || fresh->detach.request || fresh->detach.ack)
        return false;
    region=fresh;
    generation=next;
    detached=false;
    __builtin_memset(&gpu,0,sizeof(gpu));
    return initialize();
}
bool aos_vmm_virtio_gpu_quiesce(void)
{
    return !gpu.device.funs || virtio_gpu_quiesce(&gpu);
}

bool aos_vmm_virtio_gpu_detach(void)
{
    if (detached) return true;
    if (!aos_vmm_virtio_gpu_quiesce()) return false;
    attempted = true;
    if (!region) region=(void *)(AOS_FB_SHMEM_VA+client*AOS_FB_CLIENT_STRIDE);
    __atomic_store_n(&region->detach.version, AOS_FB_DETACH_VERSION, __ATOMIC_RELAXED);
    __atomic_store_n(&region->detach.request, 1u, __ATOMIC_RELEASE);
    seL4_Signal(PD_CNODE_SLOT_FB_PEER_NOTIFY);
    if (__atomic_load_n(&region->detach.ack, __ATOMIC_ACQUIRE) != 1u) return false;
    framebuffer.region = NULL;
    detached = true;
    return true;
}
