/* SPDX-License-Identifier: BSD-2-Clause */
#include <libvmm/virtio/input.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/gpa.h>
#include <libvmm/virq.h>
#include <libvmm/arch/aarch64/fault.h>
#include <platform/input.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

bool fault_is_read(uint64_t fsr) { return !(fsr&1); }
uint64_t fault_get_data_mask(uint64_t offset,uint64_t fsr)
{ return (fsr>=2 ? UINT64_C(0xff) : UINT64_C(0xffffffff)) << ((offset&3u)*8u); }
uint64_t fault_get_data(seL4_UserContext *r,uint64_t fsr) { (void)fsr; return r->x0; }
void fault_emulate_write(seL4_UserContext *r,size_t offset,size_t fsr,size_t value)
{ (void)fsr; r->x0=value >> ((offset&3u)*8u); }
bool fault_register_vm_exception_handler(uintptr_t base,size_t size,vm_exception_handler_t cb,void *data)
{ (void)base; (void)size; (void)cb; (void)data; return true; }
static unsigned interrupts;
bool virq_register(size_t cpu,size_t irq,virq_ack_fn_t ack,void *data)
{ (void)cpu; (void)irq; (void)ack; (void)data; return true; }
bool virq_inject(int irq) { (void)irq; ++interrupts; return true; }
bool virq_inject_vcpu(size_t cpu,int irq) { (void)cpu; return virq_inject(irq); }

static uint8_t memory[4096];
static void *translate(uint64_t gpa,size_t length)
{
    if (gpa<0x100000 || gpa-0x100000>sizeof(memory) || length>sizeof(memory)-(gpa-0x100000)) return NULL;
    return memory+(gpa-0x100000);
}
static aos_input_frontend_t frontend;
static aos_input_client_region_t clients[2];
static aos_input_service_t service;
static unsigned received;
static bool receive(void *context,uint8_t out[8])
{
    aos_input_event_t event;
    if (aos_input_event_receive(context,&event)!=0) return false;
    out[0]=(uint8_t)event.type; out[1]=(uint8_t)(event.type>>8);
    out[2]=(uint8_t)event.code; out[3]=(uint8_t)(event.code>>8);
    for (unsigned i=0;i<4;++i) out[4+i]=(uint8_t)((uint32_t)event.value>>(8*i));
    ++received;
    return true;
}
static void write_reg(virtio_input_device_t *s,unsigned offset,unsigned fsr,uint32_t value)
{
    seL4_UserContext r={.x0=value};
    assert(virtio_mmio_fault_handle(0,offset,fsr,&r,&s->device));
}
static uint32_t read_reg(virtio_input_device_t *s,unsigned offset,unsigned fsr)
{
    seL4_UserContext r={0};
    assert(virtio_mmio_fault_handle(0,offset,fsr,&r,&s->device));
    return (uint32_t)r.x0;
}
static uint8_t config_byte(virtio_input_device_t *s,unsigned offset)
{ return (uint8_t)read_reg(s,REG_VIRTIO_MMIO_CONFIG+offset,2); }
static unsigned select_config(virtio_input_device_t *s,unsigned select,unsigned subsel)
{
    write_reg(s,REG_VIRTIO_MMIO_CONFIG+1,3,subsel);
    write_reg(s,REG_VIRTIO_MMIO_CONFIG,3,select);
    return config_byte(s,2);
}
static void initialize(virtio_input_device_t *s,enum virtio_input_kind kind,unsigned client)
{
    assert(virtio_mmio_input_init(s,kind,receive,&clients[client].devices[kind],0xa050000,0x1000,55));
    assert(read_reg(s,REG_VIRTIO_MMIO_DEVICE_ID,0)==18);
    write_reg(s,REG_VIRTIO_MMIO_DEVICE_FEATURES_SEL,1,0);
    assert(read_reg(s,REG_VIRTIO_MMIO_DEVICE_FEATURES,0)==0);
    write_reg(s,REG_VIRTIO_MMIO_DEVICE_FEATURES_SEL,1,1);
    assert(read_reg(s,REG_VIRTIO_MMIO_DEVICE_FEATURES,0)==1);
    write_reg(s,REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL,1,0);
    write_reg(s,REG_VIRTIO_MMIO_DRIVER_FEATURES,1,0);
    write_reg(s,REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL,1,1);
    write_reg(s,REG_VIRTIO_MMIO_DRIVER_FEATURES,1,1);
    assert(s->device.features_happy);
    write_reg(s,REG_VIRTIO_MMIO_STATUS,1,VIRTIO_CONFIG_S_DRIVER_OK);
}
static void configure_queue(virtio_input_device_t *s,unsigned index)
{
    unsigned offset=index*1024;
    write_reg(s,REG_VIRTIO_MMIO_QUEUE_SEL,1,index);
    write_reg(s,REG_VIRTIO_MMIO_QUEUE_NUM,1,4);
    write_reg(s,REG_VIRTIO_MMIO_QUEUE_DESC_LOW,1,0x100000+offset);
    write_reg(s,REG_VIRTIO_MMIO_QUEUE_AVAIL_LOW,1,0x100100+offset);
    write_reg(s,REG_VIRTIO_MMIO_QUEUE_USED_LOW,1,0x100200+offset);
    write_reg(s,REG_VIRTIO_MMIO_QUEUE_READY,1,1);
    assert(s->queues[index].ready);
}
static void submit(unsigned client,unsigned device,const aos_input_event_t *events,unsigned count)
{
    aos_input_request_t q={.version=1,.id=17,.client=client,.device=device,.count=count};
    memcpy(q.events,events,count*sizeof(*events));
    assert(aos_input_submit(&frontend,&q)==0);
    uint32_t ready;
    assert(aos_input_pump(&service,&ready)==1 && ready==(1u<<client));
    aos_input_response_t p;
    assert(aos_input_receive(&frontend,&p)==0 && p.status==AOS_INPUT_OK && p.accepted==count);
}
static void fresh(virtio_input_device_t *s,enum virtio_input_kind kind)
{
    memset(memory,0,sizeof(memory)); memset(clients,0,sizeof(clients));
    received=interrupts=0;
    initialize(s,kind,0); configure_queue(s,0); configure_queue(s,1);
}
int main(void)
{
    virtio_gpa_set_translate(translate);
    aos_input_client_region_t *pages[]={&clients[0],&clients[1]};
    assert(aos_input_service_init(&service,&frontend,pages,3)==0);
    virtio_input_device_t s;
    for (unsigned kind=0;kind<2;++kind) {
        initialize(&s,(enum virtio_input_kind)kind,0);
        const char *name=kind ? "agentOS pointer" : "agentOS keyboard";
        assert(select_config(&s,1,0)==strlen(name));
        for (unsigned i=0;i<strlen(name);++i) assert(config_byte(&s,8+i)==(uint8_t)name[i]);
        assert(select_config(&s,1,1)==0);
        assert(select_config(&s,3,0)==8 && config_byte(&s,8)==6 && config_byte(&s,12)==kind+1);
        assert(select_config(&s,0x11,1)==(kind ? 35u : 32u));
        for (unsigned code=0;code<1024;++code) {
            bool advertised=(config_byte(&s,8+code/8)>>(code%8))&1;
            bool expected=kind ? (code>=0x110 && code<=0x117) : (code>=1 && code<=255);
            assert(advertised==expected);
        }
        assert(select_config(&s,0x11,2)==(kind ? 2u : 0u));
        assert(config_byte(&s,8)==(kind ? 0x43u : 0u));
        assert(config_byte(&s,9)==(kind ? 1u : 0u));
        assert(select_config(&s,0x11,0)==1 && config_byte(&s,8)==1);
        assert(select_config(&s,0x11,0x11)==0); /* no LEDs */
        assert(select_config(&s,0x12,0)==0); /* no absolute axes */
        assert(select_config(&s,0xff,0xff)==0);
        seL4_UserContext reg={.x0=1};
        assert(!virtio_mmio_fault_handle(0,REG_VIRTIO_MMIO_CONFIG+2,3,&reg,&s.device));
    }
    fresh(&s,VIRTIO_INPUT_KEYBOARD);
    const aos_input_event_t keys[]={{1,30,1},{1,30,0},{0,0,0}};
    submit(0,0,keys,3);
    submit(1,0,keys,3); /* another guest's stream must remain untouched */
    assert(virtio_input_drain(&s) && received==0 && interrupts==0);
    struct virtq *q=&s.queues[0].virtq;
    q->desc[0]=(struct virtq_desc){0x100c00,3,3,1};
    q->desc[1]=(struct virtq_desc){0x100c10,5,2,0};
    q->desc[2]=(struct virtq_desc){0x100c20,8,2,0};
    q->avail->ring[0]=0; q->avail->ring[1]=2; q->avail->idx=2;
    write_reg(&s,REG_VIRTIO_MMIO_QUEUE_NOTIFY,1,0);
    const uint8_t pressed[]={1,0,30,0,1,0,0,0};
    assert(!memcmp(memory+0xc00,pressed,3) && !memcmp(memory+0xc10,pressed+3,5));
    assert(!memcmp(memory+0xc20,(uint8_t[]){1,0,30,0,0,0,0,0},8));
    assert(received==2 && q->used->idx==2 && q->used->ring[0].id==0 && q->used->ring[0].len==8);
    assert(clients[0].devices[0].head==2 && clients[1].devices[0].head==0);
    assert(virtio_input_drain(&s) && received==2); /* no room: retain SYN */
    q->avail->ring[2]=2; q->avail->idx=3;
    assert(virtio_input_drain(&s) && received==3 && q->used->idx==3);
    assert(!memcmp(memory+0xc20,(uint8_t[8]){0},8));
    struct virtq *status=&s.queues[1].virtq;
    status->desc[0]=(struct virtq_desc){0x100c40,8,0,0};
    status->avail->ring[0]=0; status->avail->idx=1;
    write_reg(&s,REG_VIRTIO_MMIO_QUEUE_NOTIFY,1,1);
    assert(status->used->idx==1 && status->used->ring[0].len==0 && received==3);

    /* Actual queue counters wrap independently of the shared used index. */
    fresh(&s,VIRTIO_INPUT_POINTER);
    const aos_input_event_t motion[]={{2,0,-17},{0,0,0}};
    submit(0,1,motion,2);
    q=&s.queues[0].virtq;
    s.consumed[0]=s.produced[0]=UINT16_MAX;
    q->desc[0]=(struct virtq_desc){0x100c00,8,2,0};
    q->desc[1]=(struct virtq_desc){0x100c10,8,2,0};
    q->avail->ring[3]=0; q->avail->ring[0]=1; q->avail->idx=1;
    q->used->idx=77; /* shared output cannot select the private write index */
    assert(virtio_input_drain(&s) && received==2 && q->used->idx==1);
    assert(q->used->ring[3].id==0 && q->used->ring[0].id==1);
    assert(!memcmp(memory+0xc00,(uint8_t[]){2,0,0,0,0xef,0xff,0xff,0xff},8));

    /* Rejected buffers consume no input; a reset permits a fresh mapping. */
    for (unsigned bad=0;bad<5;++bad) {
        fresh(&s,VIRTIO_INPUT_KEYBOARD); submit(0,0,keys,3);
        q=&s.queues[0].virtq;
        q->desc[0]=(struct virtq_desc){0x100c00,8,2,0};
        q->avail->ring[0]=0; q->avail->idx=1;
        if (bad==0) q->desc[0].len=7;
        if (bad==1) q->desc[0].addr=0x101000;
        if (bad==2) q->desc[0].flags=0;
        if (bad==3) q->desc[0].flags=3; /* cyclic direct chain */
        if (bad==4) q->avail->idx=5;
        assert(virtio_input_drain(&s) && s.failed && received==0);
        assert(s.device.regs.Status & VIRTIO_CONFIG_S_NEEDS_RESET);
        assert(!virtio_input_drain(&s) && received==0);
        write_reg(&s,REG_VIRTIO_MMIO_STATUS,1,0);
        assert(!s.failed && !s.queues[0].ready && !s.device.regs.InterruptStatus);
        assert(clients[0].devices[0].head==0 && clients[0].devices[0].tail==3);
    }
    fresh(&s,VIRTIO_INPUT_KEYBOARD);
    submit(0,0,keys,3);
    submit(1,0,keys,3);
    s.held_event=true;
    memset(s.event,0xab,sizeof(s.event));
    virtio_input_quiesce(&s);
    virtio_input_quiesce(&s);
    assert(!s.held_event && !memcmp(s.event,(uint8_t[8]){0},8));
    assert(!virtio_input_drain(&s));
    write_reg(&s,REG_VIRTIO_MMIO_STATUS,1,0);
    s.device.regs.Status=VIRTIO_CONFIG_S_DRIVER_OK;
    s.queues[0].ready=true; /* ring pointers remain NULL after reset */
    assert(!virtio_input_drain(&s));
    seL4_UserContext late={.x0=0};
    assert(!virtio_mmio_fault_handle(0,REG_VIRTIO_MMIO_QUEUE_NOTIFY,1,&late,&s.device));
    assert(!received && !interrupts);
    assert(clients[0].devices[0].head==0 && clients[1].devices[0].head==0);
    puts("PASS: actual virtio-input discovery, exact events, isolation, backpressure, reset and quiescence");
    return 0;
}
