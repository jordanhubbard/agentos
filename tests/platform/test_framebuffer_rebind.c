#define _GNU_SOURCE
#include <platform/framebuffer.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static void *pages(size_t bytes)
{
    void *p=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(p!=MAP_FAILED);
    return p;
}
static aos_fb_response_t call(aos_fb_client_t *c, uint32_t op, uint64_t handle)
{
    aos_fb_request_t q={.version=AOS_FB_VERSION,.operation=op,.handle=handle,
        .width=2,.height=2};
    aos_fb_response_t p;
    assert(!aos_fb_submit(c->region,&q));
    assert(aos_fb_pump(c)==1);
    assert(!aos_fb_receive(c->region,&p));
    return p;
}
static void retire(aos_fb_client_t *c)
{
    aos_fb_region_t *r=c->region;
    r->detach.version=AOS_FB_DETACH_VERSION;
    r->detach.request=1;
    assert(aos_fb_pump(c)==1 && r->detach.ack==1 && c->retired);
}
int main(void)
{
    aos_fb_client_t client, peer;
    aos_fb_region_t *old=pages(AOS_FB_CLIENT_STRIDE), *fresh=pages(AOS_FB_CLIENT_STRIDE);
    uint8_t *old_arena=pages(AOS_FB_ARENA_BYTES), *arena=pages(AOS_FB_ARENA_BYTES);
    aos_fb_region_t *peer_queue=pages(AOS_FB_CLIENT_STRIDE);
    uint8_t *peer_arena=pages(AOS_FB_ARENA_BYTES);
    assert(!aos_fb_client_init(&client,old,old_arena,AOS_FB_ARENA_BYTES));
    assert(!aos_fb_client_init(&peer,peer_queue,peer_arena,AOS_FB_ARENA_BYTES));
    aos_fb_response_t original=call(&client,AOS_FB_CREATE,0);
    assert(original.status==AOS_FB_OK && original.handle==1);
    aos_fb_response_t peer_surface=call(&peer,AOS_FB_CREATE,0);
    assert(aos_fb_client_rebind(NULL,fresh,arena,AOS_FB_ARENA_BYTES,1)==-1);
    assert(aos_fb_client_rebind(&client,fresh,arena,AOS_FB_ARENA_BYTES,1)==-1);
    retire(&client);
    aos_fb_client_t retired=client;
    assert(!mprotect(old,AOS_FB_CLIENT_STRIDE,PROT_NONE));
    assert(!mprotect(old_arena,AOS_FB_ARENA_BYTES,PROT_NONE));
    fresh->req_tail=1;
    assert(aos_fb_client_rebind(&client,fresh,arena,AOS_FB_ARENA_BYTES,1)==-1);
    fresh->req_tail=0;
    assert(aos_fb_client_rebind(&client,fresh,arena,AOS_FB_ARENA_BYTES-1,1)==-1);
    assert(aos_fb_client_rebind(&client,fresh,arena,AOS_FB_ARENA_BYTES,0)==-1);
    assert(aos_fb_client_rebind(&client,fresh,arena,AOS_FB_ARENA_BYTES,2)==-1);
    assert(!memcmp(&retired,&client,sizeof(client)));
    assert(!aos_fb_client_rebind(&client,fresh,arena,AOS_FB_ARENA_BYTES,1));
    assert(client.generation==1 && !client.retired);
    assert(call(&client,AOS_FB_STATUS,original.handle).status==AOS_FB_BAD_HANDLE);
    aos_fb_response_t replacement=call(&client,AOS_FB_CREATE,0);
    assert(replacement.status==AOS_FB_OK && replacement.handle>original.handle);
    assert(call(&peer,AOS_FB_STATUS,peer_surface.handle).status==AOS_FB_OK);
    retire(&client);
    memset(fresh,0,sizeof(*fresh));
    assert(aos_fb_client_rebind(&client,fresh,arena,AOS_FB_ARENA_BYTES,1)==-1);
    assert(!aos_fb_client_rebind(&client,fresh,arena,AOS_FB_ARENA_BYTES,2));
    assert(call(&client,AOS_FB_STATUS,replacement.handle).status==AOS_FB_BAD_HANDLE);
    client.next_handle=0; /* Exhaustion must never turn into identity reuse. */
    retire(&client);
    memset(fresh,0,sizeof(*fresh));
    assert(!aos_fb_client_rebind(&client,fresh,arena,AOS_FB_ARENA_BYTES,3));
    assert(call(&client,AOS_FB_CREATE,0).status==AOS_FB_NO_SPACE);
    retire(&client);
    client.generation=UINT32_MAX;
    memset(fresh,0,sizeof(*fresh));
    assert(aos_fb_client_rebind(&client,fresh,arena,AOS_FB_ARENA_BYTES,1)==-1);
    assert(!munmap(old,AOS_FB_CLIENT_STRIDE) && !munmap(old_arena,AOS_FB_ARENA_BYTES));
    assert(!munmap(fresh,AOS_FB_CLIENT_STRIDE) && !munmap(arena,AOS_FB_ARENA_BYTES));
    assert(!munmap(peer_queue,AOS_FB_CLIENT_STRIDE) && !munmap(peer_arena,AOS_FB_ARENA_BYTES));
    puts("PASS: framebuffer generations preserve identity, retire old mappings and leave peer usable");
}
