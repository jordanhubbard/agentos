#include <libvmm/virtio/console_rx_ring.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
typedef struct { uint8_t memory[64]; uint32_t consumed; } fixture_t;
static void *map(void *context, uint64_t address, uint32_t length)
{
    fixture_t *f=context;
    if (address>sizeof(f->memory) || length>sizeof(f->memory)-address) return NULL;
    return f->memory+address;
}
static void take(void *context, void *destination, uint32_t length)
{
    fixture_t *f=context;
    for (uint32_t i=0;i<length;i++) ((uint8_t *)destination)[i]=(uint8_t)('a'+f->consumed++);
}
int main(void)
{
    struct virtq_desc descriptors[4]={0};
    uint16_t available[7]={0};
    uint64_t used_storage[6]={0};
    struct virtq ring={.num=4,.desc=descriptors,.avail=(void *)available,.used=(void *)used_storage};
    fixture_t f={0};
    virtio_console_rx_state_t state={0};
    uint16_t last=0;
    const virtio_console_rx_ops_t ops={map,take,&f};
    descriptors[0]=(struct virtq_desc){.addr=0,.len=2,.flags=3,.next=1};
    descriptors[1]=(struct virtq_desc){.addr=8,.len=4,.flags=2};
    ring.avail->ring[0]=0; ring.avail->idx=1;
    virtio_console_rx_result_t r=virtio_console_rx_ring_run(&ring,&last,&state,0,&ops);
    CHECK(r.valid && !r.completed && !f.consumed && last==0);
    r=virtio_console_rx_ring_run(&ring,&last,&state,5,&ops);
    CHECK(r.valid && r.completed==1 && r.bytes==5 && f.consumed==5);
    CHECK(!memcmp(f.memory,"ab",2) && !memcmp(f.memory+8,"cde",3));
    CHECK(last==1 && state.used==1 && ring.used->idx==1);
    CHECK(ring.used->ring[0].id==0 && ring.used->ring[0].len==5);
    r=virtio_console_rx_ring_run(&ring,&last,&state,3,&ops);
    CHECK(r.valid && !r.completed && f.consumed==5);

    /* Each invalid chain is preflighted before input or completion changes. */
    for (unsigned scenario=0;scenario<6;scenario++) {
        memset(descriptors,0,sizeof(descriptors)); memset(available,0,sizeof(available));
        memset(used_storage,0,sizeof(used_storage)); memset(&f,0,sizeof(f));
        state=(virtio_console_rx_state_t){0}; last=0;
        descriptors[0]=(struct virtq_desc){.addr=0,.len=2,.flags=3,.next=1};
        descriptors[1]=(struct virtq_desc){.addr=8,.len=4,.flags=2};
        ring.avail->idx=1;
        if (scenario==0) descriptors[1].flags=0;
        if (scenario==1) descriptors[1].flags=6;
        if (scenario==2) descriptors[1].addr=sizeof(f.memory);
        if (scenario==3) descriptors[0].next=4;
        if (scenario==4) { descriptors[1].flags=3; descriptors[1].next=0; }
        if (scenario==5) ring.avail->idx=5;
        r=virtio_console_rx_ring_run(&ring,&last,&state,5,&ops);
        CHECK(!r.valid && state.failed && !r.completed && !f.consumed && !last && !ring.used->idx);
        CHECK(f.memory[0]==0 && f.memory[8]==0);
    }
    memset(available,0,sizeof(available)); memset(used_storage,0,sizeof(used_storage));
    state=(virtio_console_rx_state_t){.used=UINT16_MAX}; last=UINT16_MAX;
    ring.used->idx=UINT16_MAX; ring.avail->idx=0; ring.avail->ring[3]=0;
    descriptors[0]=(struct virtq_desc){.addr=0,.len=2,.flags=2};
    r=virtio_console_rx_ring_run(&ring,&last,&state,2,&ops);
    CHECK(r.valid && r.completed==1 && last==0 && state.used==0 && ring.used->idx==0);
    ring.used->idx=1;
    r=virtio_console_rx_ring_run(&ring,&last,&state,1,&ops);
    CHECK(!r.valid && state.failed);
    puts("PASS: receive chain preflight, input retention, publication and index wrap");
    return 0;
}
