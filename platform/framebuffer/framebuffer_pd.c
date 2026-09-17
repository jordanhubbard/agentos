#include <platform/framebuffer.h>
#include <platform/framebuffer_observer.h>
#include "system_desc.h"
#include <sel4/sel4.h>

static aos_fb_client_t clients[AOS_FB_CLIENTS];
#ifdef AGENTOS_GUEST_GRAPHICS
static aos_fb_observer_t observer;
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
#ifdef AGENTOS_GUEST_GRAPHICS
    if (aos_fb_observer_init(&observer, (void *)AOS_FB_OBSERVER_VA, clients,
            AOS_FB_CLIENTS, (1u << AOS_FB_CLIENTS) - 1u,
            (void *)AOS_FB_SNAPSHOT_VA, AOS_FB_SURFACE_BYTES) != 0)
        for (;;) seL4_Yield();
#endif
    for (;;) {
        unsigned progress = 0;
        for (unsigned i = 0; i < AOS_FB_CLIENTS; ++i) {
            unsigned count = aos_fb_pump(&clients[i]);
            if (count) seL4_Signal(PD_CNODE_SLOT_FB_PEER_NOTIFY + i);
            progress += count;
        }
#ifdef AGENTOS_GUEST_GRAPHICS
        unsigned observed = aos_fb_observer_pump(&observer);
        if (observed)
            seL4_Signal(PD_CNODE_SLOT_FB_PEER_NOTIFY + AOS_FB_OBSERVER_CLIENT);
        progress += observed;
#endif
        if (progress) seL4_Yield();
        else { seL4_Word badge; seL4_Wait(PD_CNODE_SLOT_FB_WAIT, &badge); }
    }
}
