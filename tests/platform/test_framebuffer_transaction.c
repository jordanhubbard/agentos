#include <platform/framebuffer_rebind.h>
#include <platform/framebuffer_rebind_client.h>
#include "contracts/virtualizer_authority.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    fb_rebind_req_t request={FB_REBIND_VERSION,0,1,0};
    fb_rebind_reply_t reply={FB_REBIND_OK,FB_REBIND_VERSION,1,0};
    assert(aos_fb_rebind_reply_valid(FB_REBIND_STAGE,&request,&reply,sizeof(reply),1,0));
    assert(!aos_fb_rebind_reply_valid(FB_REBIND_STAGE,&request,&reply,sizeof(reply),0,0));
    assert(!aos_fb_rebind_reply_valid(FB_REBIND_STAGE,&request,&reply,sizeof(reply),1,1));
    assert(!aos_fb_rebind_reply_valid(FB_REBIND_STAGE,&request,&reply,sizeof(reply)-1,1,0));
    assert(!aos_fb_rebind_reply_valid(FB_REBIND_STAGE,&request,NULL,sizeof(reply),1,0));
    assert(!aos_fb_rebind_reply_valid(0,&request,&reply,sizeof(reply),1,0));
    reply.generation++;
    assert(!aos_fb_rebind_reply_valid(FB_REBIND_STAGE,&request,&reply,sizeof(reply),1,0));
    reply.generation--;
    reply.index++;
    assert(!aos_fb_rebind_reply_valid(FB_REBIND_STAGE,&request,&reply,sizeof(reply),1,0));
    reply.index--;
    reply.version++;
    assert(!aos_fb_rebind_reply_valid(FB_REBIND_STAGE,&request,&reply,sizeof(reply),1,0));
    reply.version--;
    for (uint32_t op=FB_REBIND_STAGE;op<=FB_REBIND_ABORT;op++) {
        for (uint32_t status=FB_REBIND_OK;status<=FB_REBIND_RESOURCE;status++) {
            reply.status=status;
            unsigned caps=op==FB_REBIND_STAGE && status==FB_REBIND_OK;
            assert(aos_fb_rebind_reply_valid(op,&request,&reply,sizeof(reply),caps,0));
            assert(!aos_fb_rebind_reply_valid(op,&request,&reply,sizeof(reply),!caps,0));
        }
    }
    reply.status=FB_REBIND_RESOURCE+1;
    assert(!aos_fb_rebind_reply_valid(FB_REBIND_STAGE,&request,&reply,sizeof(reply),0,0));
    aos_fb_client_t c={.retired=1,.next_handle=19};
    aos_fb_rebind_t stage={0};
    fb_rebind_req_t q={FB_REBIND_VERSION,0,1,0};
    uint64_t badge=virt_client_badge(0);
    aos_fb_region_t *queue=calloc(1,sizeof(*queue));
    uint8_t *arena=calloc(1,AOS_FB_ARENA_BYTES);
    assert(queue && arena);
    /* Abort at every possible successful mapping prefix. No partial client
     * is visible, and consumed generations cannot be replayed. */
    for (unsigned prefix=1;prefix<=FB_REBIND_FRAMES;prefix++) {
        q.generation=prefix;
        q.index=0;
        assert(aos_fb_rebind_validate(&c,&stage,badge,FB_REBIND_STAGE,&q,sizeof(q)-1)==FB_REBIND_BAD_REQUEST);
        assert(aos_fb_rebind_staged(&c,&stage,virt_client_badge(1),&q)==FB_REBIND_DENIED);
        for (unsigned index=0;index<prefix;index++) {
            q.index=index+1;
            assert(aos_fb_rebind_staged(&c,&stage,badge,&q)==FB_REBIND_BAD_STATE);
            q.index=index;
            assert(aos_fb_rebind_staged(&c,&stage,badge,&q)==FB_REBIND_OK);
            assert(!c.region && c.retired && c.next_handle==19);
            assert(aos_fb_rebind_staged(&c,&stage,badge,&q)==FB_REBIND_BAD_STATE);
        }
        q.index=0;
        if (prefix<FB_REBIND_FRAMES)
            assert(aos_fb_rebind_commit(&c,&stage,badge,&q,queue,arena)==FB_REBIND_BAD_STATE);
        assert(aos_fb_rebind_aborted(&c,&stage,badge,&q)==FB_REBIND_OK);
        assert(!stage.next_frame && !stage.generation && c.generation==prefix && !c.region);
        assert(aos_fb_rebind_staged(&c,&stage,badge,&q)==FB_REBIND_BAD_STATE);
    }
    q.generation=FB_REBIND_FRAMES+1;
    for (q.index=0;q.index<FB_REBIND_FRAMES;q.index++)
        assert(aos_fb_rebind_staged(&c,&stage,badge,&q)==FB_REBIND_OK);
    q.index=0;
    queue->req_tail=1;
    assert(aos_fb_rebind_commit(&c,&stage,badge,&q,queue,arena)==FB_REBIND_RESOURCE);
    assert(c.retired && !c.region && stage.next_frame==FB_REBIND_FRAMES);
    queue->req_tail=0;
    assert(aos_fb_rebind_commit(&c,&stage,badge,&q,queue,arena)==FB_REBIND_OK);
    assert(c.region==queue && !c.retired && c.next_handle==19 && c.generation==q.generation);
    assert(!stage.next_frame && !stage.generation);
    assert(aos_fb_rebind_aborted(&c,&stage,badge,&q)==FB_REBIND_BAD_STATE);
    assert(aos_fb_rebind_staged(&c,&stage,badge,&q)==FB_REBIND_BAD_STATE);
    free(queue);
    free(arena);
    puts("PASS: graphics transactions reject partial commits, wrong owners, out-of-order stages and replay");
}
