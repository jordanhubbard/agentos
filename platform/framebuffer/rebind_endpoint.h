#pragma once
#include <platform/framebuffer.h>
#include <sel4/sel4.h>
/* Called only between client pumps/observer copies/display forwarding. */
void aos_fb_rebind_receive(aos_fb_client_t clients[AOS_FB_CLIENTS], seL4_CPtr endpoint);
