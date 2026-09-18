/* SPDX-License-Identifier: BSD-2-Clause */
#include <libvmm/virtio/input.h>
#include <libvmm/virtio/gpa.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virq.h>
#include <string.h>

static virtio_input_device_t *state(virtio_device_t *d) { return d->device_data; }
static void reset(virtio_device_t *d)
{
    virtio_input_device_t *s=state(d);
    for (unsigned i=0; i<2; ++i) virtio_queue_reset_guest_rings(&s->queues[i]);
    memset(s->consumed,0,sizeof(s->consumed));
    memset(s->produced,0,sizeof(s->produced));
    memset(s->accepted_features,0,sizeof(s->accepted_features));
    s->select=s->subsel=0;
    s->failed=false;
    /* Keep an event already removed from the source until output succeeds.
     * Resetting this device must not clear the live virtualizer's queues. */
    d->features_happy=false;
}
static bool features(virtio_device_t *d, uint32_t *out)
{
    *out=d->regs.DeviceFeaturesSel==1 ? 1u : 0u;
    return true;
}
static bool accept_features(virtio_device_t *d, uint32_t value)
{
    virtio_input_device_t *s=state(d);
    unsigned page=d->regs.DriverFeaturesSel;
    if (page>1 || (page==0 && value) || (page==1 && value!=1)) {
        d->features_happy=false;
        return false;
    }
    s->accepted_features[page]=value;
    d->features_happy=s->accepted_features[0]==0 && s->accepted_features[1]==1;
    return true;
}
static void config(const virtio_input_device_t *s, uint8_t out[136])
{
    memset(out,0,136);
    out[0]=s->select; out[1]=s->subsel;
    uint8_t *p=out+8;
    if (s->select==1 && !s->subsel) {
        const char *name=s->kind==VIRTIO_INPUT_KEYBOARD ? "agentOS keyboard" : "agentOS pointer";
        out[2]=(uint8_t)strlen(name);
        memcpy(p,name,out[2]);
    } else if (s->select==2 && !s->subsel) {
        const char *serial=s->kind==VIRTIO_INPUT_KEYBOARD ? "agentos-keyboard-0" : "agentos-pointer-0";
        out[2]=(uint8_t)strlen(serial);
        memcpy(p,serial,out[2]);
    } else if (s->select==3 && !s->subsel) {
        out[2]=8; p[0]=6; /* BUS_VIRTUAL, no physical vendor identity */
        p[4]=(uint8_t)s->kind+1; p[6]=1;
    } else if (s->select==0x11) {
        if (s->subsel==0) { out[2]=1; p[0]=1; } /* SYN_REPORT */
        else if (s->subsel==1 && s->kind==VIRTIO_INPUT_KEYBOARD) {
            out[2]=32; memset(p,0xff,32); p[0]=0xfe; /* keys 1..255 */
        } else if (s->subsel==1) { out[2]=35; p[34]=0xff; } /* buttons 0x110..117 */
        else if (s->subsel==2 && s->kind==VIRTIO_INPUT_POINTER) {
            out[2]=2; p[0]=0x43; p[1]=1; /* REL_X/Y/HWHEEL/WHEEL */
        }
    }
    /* Unsupported selector/subselector pairs, LEDs and absolute axes have
     * size zero. No variable-sized copy depends on driver input. */
}
static bool get_config(virtio_device_t *d, uint32_t offset, uint32_t *out)
{
    if (offset>=136) return false;
    uint8_t bytes[136];
    config(state(d),bytes);
    *out=0;
    for (unsigned i=0; i<4 && offset+i<136; ++i)
        *out |= (uint32_t)bytes[offset+i] << (8u*i);
    return true;
}
static bool set_config(virtio_device_t *d, uint32_t offset, uint32_t value)
{
    if (offset>1 || value>255) return false;
    if (offset==0) state(d)->select=(uint8_t)value;
    else state(d)->subsel=(uint8_t)value;
    return true;
}

/* Snapshot a complete direct chain before touching either payload or source.
 * Only eight bytes are accessed, even if the driver offers larger buffers. */
static bool chain(virtio_input_device_t *s, struct virtq *q, unsigned head,
                  bool writable, unsigned *count)
{
    bool visited[VIRTIO_INPUT_QUEUE_SIZE]={0};
    unsigned index=head, bytes=0;
    *count=0;
    for (;;) {
        if (index>=q->num || visited[index] || *count>=q->num) return false;
        visited[index]=true;
        struct virtq_desc d=q->desc[index];
        if (d.flags & ~(VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE) ||
            !!(d.flags & VIRTQ_DESC_F_WRITE)!=writable || d.addr>UINT64_MAX-d.len)
            return false;
        unsigned take=d.len<8u-bytes ? d.len : 8u-bytes;
        if (take && !virtio_gpa_to_hva(d.addr,take)) return false;
        bytes+=take;
        s->descriptors[(*count)++]=d;
        if (!(d.flags & VIRTQ_DESC_F_NEXT)) return bytes==8;
        index=d.next;
    }
}
static bool run(virtio_input_device_t *s, unsigned queue, unsigned *completed)
{
    if (!s->queues[queue].ready) return true;
    struct virtq *q=&s->queues[queue].virtq;
    if (!q->num || q->num>VIRTIO_INPUT_QUEUE_SIZE || (q->num & (q->num-1)) ||
        !q->desc || !q->avail || !q->used) return false;
    uint16_t available=__atomic_load_n(&q->avail->idx,__ATOMIC_ACQUIRE);
    unsigned pending=(uint16_t)(available-s->consumed[queue]);
    if (pending>q->num) return false;
    for (unsigned n=0; n<pending; ++n) {
        uint16_t head=q->avail->ring[s->consumed[queue]%q->num];
        unsigned count;
        if (!chain(s,q,head,queue==0,&count)) return false;
        if (queue==0 && !s->held_event) {
            if (!s->receive(s->context,s->event)) break;
            s->held_event=true;
        }
        uint8_t ignored[8];
        unsigned copied=0;
        for (unsigned i=0; i<count && copied<8; ++i) {
            const struct virtq_desc *d=&s->descriptors[i];
            unsigned take=d->len<8u-copied ? d->len : 8u-copied;
            int error=queue==0 ? virtio_copy_to_gpa(d->addr,0,s->event+copied,take) :
                                virtio_copy_from_gpa(d->addr,0,ignored+copied,take);
            if (error) return false;
            copied+=take;
        }
        /* No LED/output capabilities are advertised, so status events have
         * no side effects. Returning their buffers remains bounded. */
        q->used->ring[s->produced[queue]%q->num]=(struct virtq_used_elem){head,queue==0 ? 8u : 0u};
        __atomic_store_n(&q->used->idx,++s->produced[queue],__ATOMIC_RELEASE);
        ++s->consumed[queue];
        if (queue==0) s->held_event=false;
        ++*completed;
    }
    return true;
}
bool virtio_input_drain(virtio_input_device_t *s)
{
    if (!s || s->quiesced || s->failed) return false;
    virtio_device_t *d=&s->device;
    if (!(d->regs.Status & VIRTIO_CONFIG_S_DRIVER_OK)) return true;
    unsigned completed=0;
    if (!run(s,0,&completed) || !run(s,1,&completed)) {
        s->failed=true;
        d->regs.Status |= VIRTIO_CONFIG_S_NEEDS_RESET;
        d->regs.InterruptStatus |= 2;
    }
    if (completed) d->regs.InterruptStatus |= 1;
    return (completed || s->failed) ? virq_inject(d->virq) : true;
}
void virtio_input_quiesce(virtio_input_device_t *s)
{
    if (!s || s->quiesced) return;
    s->quiesced=true;
    reset(&s->device);
    s->held_event=false;
    memset(s->event,0,sizeof(s->event));
    memset(s->descriptors,0,sizeof(s->descriptors));
}

static bool notify(virtio_device_t *d)
{
    if (d->regs.QueueNotify>=2) return false;
    return virtio_input_drain(state(d));
}
static virtio_device_funs_t functions={reset,features,accept_features,get_config,set_config,notify};
bool virtio_mmio_input_init(virtio_input_device_t *s, enum virtio_input_kind kind,
    virtio_input_receive_fn receive, void *context, uintptr_t base, uintptr_t size, size_t virq)
{
    if (!s || !receive || (kind!=VIRTIO_INPUT_KEYBOARD && kind!=VIRTIO_INPUT_POINTER)) return false;
    memset(s,0,sizeof(*s));
    s->kind=kind; s->receive=receive; s->context=context;
    virtio_device_t *d=&s->device;
    d->regs.DeviceID=18;
    d->regs.VendorID=VIRTIO_MMIO_DEV_VENDOR_ID;
    d->transport_type=VIRTIO_TRANSPORT_MMIO;
    d->funs=&functions; d->vqs=s->queues; d->num_vqs=2;
    d->virq=virq; d->device_data=s;
    return virtio_mmio_register_device(d,base,size,virq);
}
