#include <platform/framebuffer.h>
#include <platform/framebuffer_isolation_probe.h>
#include "system_desc.h"
#include "serial_log.h"
#include <string.h>

#ifndef FB_TEST_CLIENT
#error FB_TEST_CLIENT must select the assigned root-provisioned page
#endif
static aos_fb_region_t *const region = (void *)(AOS_FB_SHMEM_VA +
    FB_TEST_CLIENT * AOS_FB_CLIENT_STRIDE);

static void report(const char *text)
{
    serial_log_t channel = { .ep = PD_CNODE_SLOT_SERIAL_EP };
    serial_log_puts(&channel, text);
}
static void fail(void)
{
    report("[framebuffer] FAIL: client queue or pixel contract\n");
    for (;;) { seL4_Word badge; seL4_Wait(PD_CNODE_SLOT_FB_WAIT, &badge); }
}
static aos_fb_response_t call(aos_fb_request_t q)
{
    static uint32_t id;
    q.version = AOS_FB_VERSION;
    q.id = ++id;
    if (aos_fb_submit(region, &q) != 0) fail();
    seL4_Signal(PD_CNODE_SLOT_FB_PEER_NOTIFY);
    aos_fb_response_t p;
    /* Receiving a response requires an actual persistent notification. */
    do {
        seL4_Word badge;
        seL4_Wait(PD_CNODE_SLOT_FB_WAIT, &badge);
    } while (aos_fb_receive(region, &p) != 0);
    seL4_Signal(PD_CNODE_SLOT_FB_PEER_NOTIFY);
    if (p.version != AOS_FB_VERSION || p.id != q.id) fail();
    return p;
}
void pd_main(seL4_CPtr endpoint, seL4_CPtr nameserver)
{
    (void)endpoint;
    (void)nameserver;
    aos_fb_response_t p = call((aos_fb_request_t){
        .operation = AOS_FB_CREATE, .width = 8, .height = 8 });
    if (p.status != AOS_FB_OK || !p.handle || p.width != 8 || p.height != 8) fail();
    uint64_t handle = p.handle;
    uint8_t expected[256];
    for (unsigned i = 0; i < sizeof(expected); ++i)
        expected[i] = region->data[i] = (uint8_t)(i * 37u + FB_TEST_CLIENT * 83u);
    aos_fb_request_t pixels = { .operation = AOS_FB_WRITE, .handle = handle,
        .width = 8, .height = 8, .data_length = sizeof(expected) };
    if (call(pixels).status != AOS_FB_OK) fail();
    p = call((aos_fb_request_t){ .operation = AOS_FB_FLIP, .handle = handle });
    if (p.status != AOS_FB_OK || p.sequence != 1) fail();
    memset(region->data, 0, sizeof(expected));
    pixels.operation = AOS_FB_READ;
    if (call(pixels).status != AOS_FB_OK ||
        memcmp(region->data, expected, sizeof(expected))) fail();
    p = call((aos_fb_request_t){ .operation = AOS_FB_STATUS, .handle = handle });
    if (p.status != AOS_FB_OK || p.width != 8 || p.height != 8 || p.sequence != 1) fail();
    if (call((aos_fb_request_t){ .operation = AOS_FB_DESTROY,
                                .handle = handle }).status != AOS_FB_OK) fail();
    if (call(pixels).status != AOS_FB_BAD_HANDLE) fail();
#if FB_TEST_CLIENT == 0
    report("[framebuffer] PASS: client 0 create/write/flip/status/read/destroy exact pixels\n");
#else
    report("[framebuffer] PASS: client 1 create/write/flip/status/read/destroy exact pixels\n");
#endif
    /* Root, not the client, verifies and reports the denied mapping. */
#ifdef AGENTOS_FRAMEBUFFER_ISOLATION_PROBE
    if (FB_TEST_CLIENT == AOS_FB_PROBE_CLIENT) {
        volatile uint8_t *forbidden = (void *)AOS_FB_PROBE_ADDRESS;
        if (AOS_FB_PROBE_WRITE) *forbidden = 0x5a;
        else { volatile uint8_t value = *forbidden; (void)value; }
        fail();
    }
#endif
    for (;;) { seL4_Word badge; seL4_Wait(PD_CNODE_SLOT_FB_WAIT, &badge); }
}
