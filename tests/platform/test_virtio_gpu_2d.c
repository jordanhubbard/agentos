#include <platform/gpu_framebuffer.h>
#include <libvmm/virtio/gpu_ring.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static aos_fb_client_t service;
static aos_gpu_framebuffer_t adapter;
static virtio_gpu_2d_t gpu;
static uint8_t guest[AOS_FB_SURFACE_BYTES];
static uint8_t request[VIRTIO_GPU_2D_REQUEST_BYTES], response[VIRTIO_GPU_2D_RESPONSE_BYTES];
static unsigned calls;
static bool fail_exchange;

static void put32(unsigned off, uint32_t value)
{
    for (unsigned i=0; i<4; ++i) request[off+i]=(uint8_t)(value >> (8u*i));
}
static void put64(unsigned off, uint64_t value)
{
    put32(off, (uint32_t)value); put32(off+4, (uint32_t)(value >> 32));
}
static uint32_t get32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void begin(uint32_t type)
{
    memset(request,0,sizeof(request)); put32(0,type);
}
static uint32_t run(size_t length)
{
    assert(virtio_gpu_2d_execute(&gpu,request,length,response,sizeof(response)) >= 24);
    return get32(response);
}
static bool exchange(void *ctx, const aos_fb_request_t *q, aos_fb_response_t *p)
{
    (void)ctx;
    ++calls;
    if (fail_exchange) return false;
    assert(aos_fb_submit(service.region,q)==0);
    assert(aos_fb_pump(&service)==1);
    assert(aos_fb_receive(service.region,p)==0);
    return true;
}
static bool validate(void *ctx, uint64_t address, uint32_t length)
{
    (void)ctx;
    return address >= 0x80000000 && address - 0x80000000 <= sizeof(guest) &&
           length <= sizeof(guest) - (address - 0x80000000);
}
static bool read_guest(void *ctx, uint64_t address, void *out, uint32_t length)
{
    if (!validate(ctx,address,length)) return false;
    memcpy(out,guest+(address-0x80000000),length); return true;
}
static bool write_guest(void *ctx, uint64_t address, const void *in, uint32_t length)
{
    if (!validate(ctx,address,length)) return false;
    memcpy(guest+(address-0x80000000),in,length); return true;
}
static void create_resource(unsigned id)
{
    begin(GPU_RESOURCE_CREATE_2D); put32(24,id); put32(28,2); put32(32,8); put32(36,6);
    assert(run(40)==GPU_OK_NODATA);
}
static void rect_command(uint32_t type, unsigned id)
{
    begin(type); put32(24,2); put32(28,1); put32(32,3); put32(36,2);
    put32(type==GPU_TRANSFER_TO_HOST_2D ? 48 : type==GPU_SET_SCANOUT ? 44 : 40,id);
}
static void read_frame(uint8_t *out)
{
    aos_fb_response_t p;
    aos_fb_request_t q = {.version=AOS_FB_VERSION,.operation=AOS_FB_READ,
        .handle=adapter.scanout_handle,.width=8,.height=6,.data_length=8*6*4};
    assert(exchange(NULL,&q,&p) && p.status==AOS_FB_OK);
    memcpy(out,service.region->data,8*6*4);
}
static void ring_test(void)
{
    struct virtq_desc desc[4] = {
        {0x80000200,8,VIRTQ_DESC_F_NEXT,1},
        {0x80000208,16,VIRTQ_DESC_F_NEXT,2},
        {0x80000400,13,VIRTQ_DESC_F_NEXT|VIRTQ_DESC_F_WRITE,3},
        {0x80000500,395,VIRTQ_DESC_F_WRITE,0}
    };
    struct virtq ring={.num=4,.desc=desc};
    ring.avail=calloc(1,sizeof(*ring.avail)+4*sizeof(uint16_t));
    ring.used=calloc(1,sizeof(*ring.used)+4*sizeof(struct virtq_used_elem));
    virtio_gpu_ring_t *state=calloc(1,sizeof(*state));
    assert(ring.avail && ring.used && state);
    const virtio_gpu_ring_ops_t ops={validate,read_guest,write_guest,NULL};
    begin(GPU_GET_DISPLAY_INFO);
    memcpy(guest+512,request,24);
    ring.avail->idx=1;
    virtio_gpu_ring_result_t result=virtio_gpu_control_run(state,&ring,&gpu,&ops,1);
    assert(result.valid && result.completed==1 && ring.used->idx==1);
    assert(ring.used->ring[0].id==0 && ring.used->ring[0].len==408);
    memcpy(response,guest+1024,13); memcpy(response+13,guest+1280,395);
    assert(get32(response)==GPU_OK_DISPLAY_INFO && get32(response+32)==1024);
    result=virtio_gpu_control_run(state,&ring,&gpu,&ops,1);
    assert(result.valid && result.completed==0); /* no duplicate completion */

    memset(state,0,sizeof(*state));
    state->last_index=state->used_index=UINT16_MAX;
    ring.avail->ring[3]=0; ring.avail->idx=0;
    result=virtio_gpu_control_run(state,&ring,&gpu,&ops,1);
    assert(result.valid && result.completed==1 && state->last_index==0 && ring.used->idx==0);
    assert(ring.used->ring[3].len==408);
    /* A malformed chain never executes a command and stays stopped. */
    ring.avail->idx=1; desc[1].next=0;
    unsigned before=calls;
    result=virtio_gpu_control_run(state,&ring,&gpu,&ops,1);
    assert(!result.valid && state->failed && !result.completed && calls==before);
    desc[1].next=2;
    assert(!virtio_gpu_control_run(state,&ring,&gpu,&ops,1).valid);
    memset(state,0,sizeof(*state));
    desc[0].addr=UINT64_MAX-3;
    assert(!virtio_gpu_control_run(state,&ring,&gpu,&ops,1).valid);
    memset(state,0,sizeof(*state)); desc[0].addr=0x80000200;
    desc[3].len=3; /* reply cannot hold display information */
    assert(!virtio_gpu_control_run(state,&ring,&gpu,&ops,1).valid && calls==before);
    memset(state,0,sizeof(*state)); desc[3].len=395;
    ring.avail->idx=5;
    assert(!virtio_gpu_control_run(state,&ring,&gpu,&ops,1).valid);
    memset(state,0,sizeof(*state));
    begin(0x301); put32(28,51); put32(32,67);
    memcpy(guest+512,request,56);
    desc[0]=(struct virtq_desc){0x80000200,56,0,0};
    ring.avail->idx=1;
    result=virtio_gpu_cursor_run(state,&ring,&gpu,&ops,1);
    assert(result.valid && result.completed==1 && ring.used->ring[0].len==0);
    assert(!adapter.cursor_handle && adapter.cursor_x==51 && adapter.cursor_y==67);
    free(state); free(ring.avail); free(ring.used);
}
static void full_frame_test(void)
{
    const unsigned width=1024, height=768;
    begin(GPU_RESOURCE_CREATE_2D); put32(24,91); put32(28,2); put32(32,width); put32(36,height);
    assert(run(40)==GPU_OK_NODATA);
    begin(GPU_RESOURCE_ATTACH_BACKING); put32(24,91); put32(28,1);
    put64(32,0x80000000); put32(40,sizeof(guest));
    assert(run(48)==GPU_OK_NODATA);
    for (unsigned i=0;i<sizeof(guest);++i) guest[i]=(uint8_t)(i*17u+(i/4096));
    begin(GPU_SET_SCANOUT); put32(32,width); put32(36,height); put32(44,91);
    assert(run(48)==GPU_OK_NODATA);
    begin(GPU_TRANSFER_TO_HOST_2D); put32(32,width); put32(36,height); put32(48,91);
    unsigned before=calls;
    assert(run(56)==GPU_OK_NODATA && calls-before==48);
    begin(GPU_RESOURCE_FLUSH); put32(32,width); put32(36,height); put32(40,91);
    assert(run(48)==GPU_OK_NODATA);
    for (unsigned row=0;row<height;row+=16) {
        aos_fb_request_t q={.version=AOS_FB_VERSION,.operation=AOS_FB_READ,
            .handle=adapter.scanout_handle,.y=row,.width=width,.height=16,
            .data_length=65536};
        aos_fb_response_t p;
        assert(exchange(NULL,&q,&p) && p.status==AOS_FB_OK);
        assert(!memcmp(service.region->data,guest+row*width*4u,65536));
    }
    assert(virtio_gpu_2d_reset(&gpu));
}
int main(void)
{
    aos_fb_region_t *region=calloc(1,sizeof(*region));
    uint8_t *arena=malloc(AOS_FB_ARENA_BYTES);
    assert(region && arena && aos_fb_client_init(&service,region,arena,AOS_FB_ARENA_BYTES)==0);
    adapter=(aos_gpu_framebuffer_t){.region=region,.exchange=exchange,
        .validate_gpa=validate,.read_gpa=read_guest};
    assert(aos_gpu_framebuffer_init(&adapter,&gpu));

    begin(GPU_GET_DISPLAY_INFO);
    assert(virtio_gpu_2d_execute(&gpu,request,24,response,sizeof(response))==408);
    assert(get32(response)==GPU_OK_DISPLAY_INFO && get32(response+32)==1024 &&
        get32(response+36)==768 && get32(response+40)==1);
    for (unsigned i=48; i<sizeof(response); ++i) assert(response[i]==0);
    create_resource(7);
    assert(run(40)==GPU_ERR_INVALID_PARAMETER); /* duplicate cannot consume another slot */
    begin(GPU_RESOURCE_ATTACH_BACKING); put32(24,7); put32(28,2);
    put64(32,0x80000000); put32(40,44); put64(48,0x80000100); put32(56,148);
    assert(run(64)==GPU_OK_NODATA);
    /* Two noncontiguous pages with a split inside the first copied row. */
    for (unsigned i=0; i<44; ++i) guest[i]=(uint8_t)i;
    for (unsigned i=0; i<148; ++i) guest[256+i]=(uint8_t)(44+i);
    rect_command(GPU_SET_SCANOUT,7); assert(run(48)==GPU_OK_NODATA);
    rect_command(GPU_TRANSFER_TO_HOST_2D,7); put64(40,40);
    assert(run(56)==GPU_OK_NODATA);
    uint8_t actual[8*6*4], expected[8*6*4]={0};
    read_frame(actual);
    assert(memcmp(actual,expected,sizeof(actual))==0); /* transfer is not a flip */
    rect_command(GPU_RESOURCE_FLUSH,7); put32(4,1); put64(8,0x123456789abcdef0ULL);
    assert(run(48)==GPU_OK_NODATA && get32(response+4)==1 && memcmp(response+8,request+8,8)==0);
    for (unsigned row=0; row<2; ++row)
        for (unsigned i=0; i<12; ++i) expected[((row+1)*8+2)*4+i]=(uint8_t)(40+row*32+i);
    read_frame(actual); assert(memcmp(actual,expected,sizeof(actual))==0);

    unsigned before=calls;
    rect_command(GPU_TRANSFER_TO_HOST_2D,7); put64(40,UINT64_MAX);
    assert(run(56)==GPU_ERR_INVALID_PARAMETER && calls==before);
    put64(40,40); put32(24,UINT32_MAX);
    assert(run(56)==GPU_ERR_INVALID_PARAMETER && calls==before);
    begin(GPU_RESOURCE_UNREF); put32(24,7);
    assert(virtio_gpu_2d_execute(&gpu,request,32,response,23)==0 && calls==before);
    fail_exchange=true;
    assert(run(32)==GPU_ERR_UNSPEC); /* retain resource on backend failure */
    fail_exchange=false;
    assert(run(32)==GPU_OK_NODATA && adapter.scanout_handle==0);
    assert(run(32)==GPU_ERR_INVALID_RESOURCE_ID);

    create_resource(7);
    begin(GPU_RESOURCE_ATTACH_BACKING); put32(24,7); put32(28,1);
    put64(32,UINT64_MAX-4); put32(40,192);
    assert(run(48)==GPU_ERR_INVALID_PARAMETER);
    put64(32,0x90000000); assert(run(48)==GPU_ERR_INVALID_PARAMETER);
    put64(32,0x80000000); put32(40,191); assert(run(48)==GPU_ERR_INVALID_PARAMETER);
    put32(40,192); assert(run(48)==GPU_OK_NODATA);
    begin(GPU_RESOURCE_DETACH_BACKING); put32(24,7); assert(run(32)==GPU_OK_NODATA);
    rect_command(GPU_TRANSFER_TO_HOST_2D,7); assert(run(56)==GPU_ERR_INVALID_PARAMETER);
    for (unsigned i=1; i<VIRTIO_GPU_2D_RESOURCES; ++i) create_resource(i);
    begin(GPU_RESOURCE_CREATE_2D); put32(24,100); put32(28,2); put32(32,8); put32(36,6);
    assert(run(40)==GPU_ERR_OUT_OF_MEMORY);
    assert(virtio_gpu_2d_reset(&gpu));
    create_resource(7); /* reset returns backend capacity */
    assert(virtio_gpu_2d_reset(&gpu));
    ring_test();
    begin(GPU_RESOURCE_CREATE_2D); put32(24,21); put32(28,1); put32(32,64); put32(36,64);
    assert(run(40)==GPU_OK_NODATA);
    begin(GPU_RESOURCE_ATTACH_BACKING); put32(24,21); put32(28,1);
    put64(32,0x80002000); put32(40,64*64*4);
    assert(run(48)==GPU_OK_NODATA);
    for (unsigned i=0; i<64*64*4; ++i) guest[8192+i]=(uint8_t)(i*13u);
    begin(GPU_TRANSFER_TO_HOST_2D); put32(32,64); put32(36,64); put32(48,21);
    assert(run(56)==GPU_OK_NODATA);
    begin(0x300); put32(28,17); put32(32,29); put32(40,21); put32(44,3); put32(48,7);
    assert(virtio_gpu_2d_cursor(&gpu,request,56));
    assert(adapter.cursor_handle && adapter.cursor_x==17 && adapter.cursor_y==29 &&
        adapter.cursor_hot_x==3 && adapter.cursor_hot_y==7);
    uint64_t cursor_handle=adapter.cursor_handle;
    aos_fb_response_t cursor_reply;
    aos_fb_request_t cursor_read={.version=AOS_FB_VERSION,.operation=AOS_FB_READ,
        .handle=cursor_handle,.width=64,.height=64,.data_length=64*64*4};
    assert(exchange(NULL,&cursor_read,&cursor_reply) && cursor_reply.status==AOS_FB_OK);
    assert(memcmp(service.region->data,guest+8192,64*64*4)==0 && cursor_reply.sequence==1);
    begin(0x301); put32(28,30); put32(32,40);
    assert(virtio_gpu_2d_cursor(&gpu,request,56));
    assert(adapter.cursor_handle==cursor_handle && adapter.cursor_x==30 && adapter.cursor_y==40);
    assert(exchange(NULL,&cursor_read,&cursor_reply) && cursor_reply.sequence==1);
    begin(0x300); put32(40,21); put32(44,64);
    assert(!virtio_gpu_2d_cursor(&gpu,request,56) && adapter.cursor_handle==cursor_handle);
    begin(GPU_RESOURCE_UNREF); put32(24,21);
    assert(run(32)==GPU_OK_NODATA && !adapter.cursor_handle);
    assert(virtio_gpu_2d_reset(&gpu));
    full_frame_test();
    free(region); free(arena);
    puts("PASS: virtio GPU 2D commands produce exact committed framebuffer pixels; bounds, backing, fences, failure and reset");
    return 0;
}
