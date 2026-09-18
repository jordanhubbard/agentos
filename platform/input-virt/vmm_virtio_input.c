/* SPDX-License-Identifier: BSD-2-Clause */
#include <platform/vmm_virtio_input.h>
#include <platform/input.h>
#include <libvmm/virtio/input.h>
#include "system_desc.h"
#include <sel4/sel4.h>

static virtio_input_device_t devices[AOS_INPUT_DEVICES];
static bool initialized;
static bool attempted;
static bool detached;
static bool receive(void *context,uint8_t out[8])
{
    aos_input_event_t e;
    if (aos_input_event_receive(context,&e)!=0) return false;
    out[0]=(uint8_t)e.type; out[1]=(uint8_t)(e.type>>8);
    out[2]=(uint8_t)e.code; out[3]=(uint8_t)(e.code>>8);
    for (unsigned i=0;i<4;++i) out[4+i]=(uint8_t)((uint32_t)e.value>>(8u*i));
    seL4_Signal(PD_CNODE_SLOT_INPUT_PEER_NOTIFY);
    return true;
}
bool aos_vmm_virtio_input_init(void)
{
    if (attempted || detached) return false;
    attempted=true;
#ifdef AGENTOS_GUEST_SECONDARY
    const unsigned client=1;
#else
    const unsigned client=0;
#endif
    aos_input_client_region_t *region=(void *)(AOS_INPUT_SHMEM_VA+client*AOS_INPUT_FRAME_SIZE);
    if (!virtio_mmio_input_init(&devices[0],VIRTIO_INPUT_KEYBOARD,receive,&region->devices[0],
            AOS_VIRTIO_INPUT_KEYBOARD_IPA,AOS_VIRTIO_INPUT_MMIO_SIZE,AOS_VIRTIO_INPUT_KEYBOARD_VIRQ) ||
        !virtio_mmio_input_init(&devices[1],VIRTIO_INPUT_POINTER,receive,&region->devices[1],
            AOS_VIRTIO_INPUT_POINTER_IPA,AOS_VIRTIO_INPUT_MMIO_SIZE,AOS_VIRTIO_INPUT_POINTER_VIRQ))
        return false;
    initialized=true;
    LOG_VMM("emulated virtio-input: keyboard/pointer client %u ready\n",client);
    return true;
}
void aos_vmm_virtio_input_drain(void)
{
    if (!initialized) return;
    for (unsigned i=0;i<AOS_INPUT_DEVICES;++i) (void)virtio_input_drain(&devices[i]);
}
void aos_vmm_virtio_input_quiesce(void)
{
    for (unsigned i=0;i<AOS_INPUT_DEVICES;++i)
        if (devices[i].device.funs) virtio_input_quiesce(&devices[i]);
}

bool aos_vmm_virtio_input_detach(void)
{
    if (detached) return true;
    aos_vmm_virtio_input_quiesce();
#ifdef AGENTOS_GUEST_SECONDARY
    const unsigned client=1;
#else
    const unsigned client=0;
#endif
    aos_input_client_region_t *region=(void *)(AOS_INPUT_SHMEM_VA+client*AOS_INPUT_FRAME_SIZE);
    __atomic_store_n(&region->detach.version,AOS_INPUT_DETACH_VERSION,__ATOMIC_RELAXED);
    __atomic_store_n(&region->detach.request,1u,__ATOMIC_RELEASE);
    seL4_Signal(PD_CNODE_SLOT_INPUT_PEER_NOTIFY);
    if (__atomic_load_n(&region->detach.ack,__ATOMIC_ACQUIRE)!=1u) return false;
    initialized=false;
    detached=true;
    return true;
}
