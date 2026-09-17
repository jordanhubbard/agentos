/* SPDX-License-Identifier: BSD-2-Clause */
#include <libvmm/virtio/gpu.h>
#include <libvmm/virtio/gpa.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virq.h>
#include <string.h>

static virtio_gpu_device_t *state(virtio_device_t *d) { return d->device_data; }
static bool validate(void *ctx, uint64_t gpa, uint32_t bytes)
{
    (void)ctx;
    return gpa <= UINT64_MAX - bytes && virtio_gpa_to_hva(gpa,bytes) != NULL;
}
static bool read_guest(void *ctx, uint64_t gpa, void *out, uint32_t bytes)
{
    (void)ctx;
    return virtio_copy_from_gpa(gpa,0,out,bytes)==0;
}
static bool write_guest(void *ctx, uint64_t gpa, const void *in, uint32_t bytes)
{
    (void)ctx;
    return virtio_copy_to_gpa(gpa,0,in,bytes)==0;
}
static void reset(virtio_device_t *d)
{
    virtio_gpu_device_t *g=state(d);
    g->reset_failed=!virtio_gpu_2d_reset(&g->engine);
    memset(g->rings,0,sizeof(g->rings));
    memset(g->accepted_features,0,sizeof(g->accepted_features));
    for (unsigned i=0; i<2; ++i) virtio_queue_reset_guest_rings(&g->queues[i]);
    d->features_happy=false;
}
static bool features(virtio_device_t *d, uint32_t *out)
{
    *out=d->regs.DeviceFeaturesSel==1 ? 1u : 0u;
    return true;
}
static bool accept_features(virtio_device_t *d, uint32_t value)
{
    virtio_gpu_device_t *g=state(d);
    unsigned page=d->regs.DriverFeaturesSel;
    if (page>1 || (page==0 && value) || (page==1 && value!=1)) {
        d->features_happy=false;
        return false;
    }
    g->accepted_features[page]=value;
    d->features_happy=g->accepted_features[0]==0 && g->accepted_features[1]==1 && !g->reset_failed;
    return true;
}
static bool get_config(virtio_device_t *d, uint32_t offset, uint32_t *out)
{
    (void)d;
    /* events_read, events_clear, num_scanouts, num_capsets */
    if (offset>12 || (offset&3)) return false;
    *out=offset==8 ? 1u : 0u;
    return true;
}
static bool set_config(virtio_device_t *d, uint32_t offset, uint32_t value)
{
    (void)d; (void)value;
    return offset==4; /* no dynamic display events to clear */
}
static bool notify(virtio_device_t *d)
{
    virtio_gpu_device_t *g=state(d);
    unsigned queue=d->regs.QueueNotify;
    if (queue>=2 || g->reset_failed || !g->queues[queue].ready ||
        !(d->regs.Status & VIRTIO_CONFIG_S_DRIVER_OK)) return false;
    const virtio_gpu_ring_ops_t ops={validate,read_guest,write_guest,NULL};
    virtio_gpu_ring_result_t result=queue==0 ?
        virtio_gpu_control_run(&g->rings[queue],&g->queues[queue].virtq,&g->engine,&ops,VIRTIO_GPU_QUEUE_SIZE) :
        virtio_gpu_cursor_run(&g->rings[queue],&g->queues[queue].virtq,&g->engine,&ops,VIRTIO_GPU_QUEUE_SIZE);
    if (!result.valid) d->regs.Status |= VIRTIO_CONFIG_S_NEEDS_RESET;
    static unsigned reported;
    if (reported < 16) {
        uint32_t command = 0, response = 0;
        if (result.completed) {
            memcpy(&command, g->rings[queue].request, sizeof(command));
            memcpy(&response, g->rings[queue].response, sizeof(response));
        }
        LOG_VMM("emulated virtio-gpu: queue=%u completed=%u valid=%u last-command=0x%x response=0x%x\n",
                queue, result.completed, result.valid, command, response);
        ++reported;
    }
    if (result.completed || !result.valid) {
        d->regs.InterruptStatus |= result.valid ? 1u : 3u;
        return virq_inject(d->virq);
    }
    return true;
}
static virtio_device_funs_t functions={reset,features,accept_features,get_config,set_config,notify};

bool virtio_mmio_gpu_init(virtio_gpu_device_t *g, uintptr_t base, uintptr_t size, size_t virq)
{
    if (!g || !g->engine.ops.create || !g->engine.ops.read_gpa) return false;
    virtio_device_t *d=&g->device;
    memset(d,0,sizeof(*d));
    memset(g->queues,0,sizeof(g->queues));
    memset(g->rings,0,sizeof(g->rings));
    memset(g->accepted_features,0,sizeof(g->accepted_features));
    g->reset_failed=false;
    d->regs.DeviceID=16;
    d->regs.VendorID=VIRTIO_MMIO_DEV_VENDOR_ID;
    d->transport_type=VIRTIO_TRANSPORT_MMIO;
    d->funs=&functions;
    d->vqs=g->queues;
    d->num_vqs=2;
    d->virq=virq;
    d->device_data=g;
    return virtio_mmio_register_device(d,base,size,virq);
}
