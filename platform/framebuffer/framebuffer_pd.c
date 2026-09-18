#include <platform/framebuffer.h>
#include <platform/framebuffer_observer.h>
#include "system_desc.h"
#include <sel4/sel4.h>

static aos_fb_client_t clients[AOS_FB_CLIENTS];
static aos_fb_observer_t observer;
#ifdef AGENTOS_DISPLAY_RAMFB
#include <platform/display_producer.h>
#include <platform/display_layout.h>
#include "serial_log.h"
static void display_failure(const char *message)
{
    static serial_log_t channel={.ep=PD_CNODE_SLOT_SERIAL_EP};
    serial_log_puts(&channel,message);
}
static int display_exchange(void *context,const aos_display_request_t *q,
                             aos_display_response_t *response)
{
    aos_display_region_t *region=context;
    if (aos_display_submit(region,q)!=0) {
        display_failure("[framebuffer] display FAIL: queue submission\n");
        return -1;
    }
    seL4_Signal(PD_CNODE_SLOT_DISPLAY_PEER_NOTIFY);
    for (unsigned attempt=0;attempt<100;++attempt) {
        if (aos_display_receive(region,response)==0) {
            seL4_Signal(PD_CNODE_SLOT_DISPLAY_PEER_NOTIFY);
            return 0;
        }
        seL4_Yield();
    }
    display_failure("[framebuffer] display FAIL: bounded reply wait\n");
    return -1;
}
static aos_display_producer_t display={
    .region=(void *)AOS_DISPLAY_QUEUE_VA,.context=(void *)AOS_DISPLAY_QUEUE_VA,
    .exchange=display_exchange};
#endif

void pd_main(seL4_CPtr endpoint, seL4_CPtr nameserver)
{
    (void)endpoint;
    (void)nameserver;
    for (unsigned i = 0; i < AOS_FB_CLIENTS; ++i)
        if (aos_fb_client_init(&clients[i], (void *)(AOS_FB_SHMEM_VA +
                i * AOS_FB_CLIENT_STRIDE),
                (void *)(AOS_FB_ARENA_VA + i * AOS_FB_ARENA_BYTES), AOS_FB_ARENA_BYTES) != 0)
            for (;;) seL4_Yield();
    if (aos_fb_observer_init(&observer, (void *)AOS_FB_OBSERVER_VA, clients,
            AOS_FB_CLIENTS, (1u << AOS_FB_CLIENTS) - 1u,
            (void *)AOS_FB_SNAPSHOT_VA, AOS_FB_SURFACE_BYTES) != 0)
        for (;;) seL4_Yield();
    for (;;) {
        unsigned progress = 0;
        for (unsigned i = 0; i < AOS_FB_CLIENTS; ++i) {
            unsigned count = aos_fb_pump(&clients[i]);
            if (count) seL4_Signal(PD_CNODE_SLOT_FB_PEER_NOTIFY + i);
            progress += count;
        }
        unsigned observed = aos_fb_observer_pump(&observer);
        if (observed)
            seL4_Signal(PD_CNODE_SLOT_FB_PEER_NOTIFY + AOS_FB_OBSERVER_CLIENT);
        progress += observed;
#ifdef AGENTOS_DISPLAY_RAMFB
        /* Fixed primary-client focus. Guests can select only their own
         * surfaces. No pump runs during forwarding, so committed pixels
         * remain stable across the bounded sequence of queue exchanges. */
        unsigned was_failed=display.failed;
        if (aos_display_forward(&display,&clients[0])>0) ++progress;
        if (!was_failed && display.failed)
            display_failure("[framebuffer] display disabled after uncertain or rejected transaction\n");
#endif
        /* Each pump has a fixed request budget. Continue while work exists;
         * Yield on a periodic MCS context forfeits its remaining budget and
         * would delay every GPU row until the next scheduling period. The
         * kernel still enforces the service's configured CPU budget. */
        if (!progress) { seL4_Word badge; seL4_Wait(PD_CNODE_SLOT_FB_WAIT, &badge); }
    }
}
