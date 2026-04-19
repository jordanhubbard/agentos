/*
 * agentOS framebuffer_contract — Contract Unit Tests
 *
 * Validates all MSG_FB_* opcodes, struct layouts, backend constants, pixel
 * format constants, and error codes without seL4 or Microkit.  No
 * framebuffer_pd implementation is tested — the tests verify only that the
 * API definitions in framebuffer_contract.h are internally consistent and
 * compile cleanly on a host.
 *
 * Build:  cc -o /tmp/test_framebuffer_contract \
 *             tests/test_framebuffer_contract.c \
 *             -I tests \
 *             -I kernel/agentos-root-task/include \
 *             -DAGENTOS_TEST_HOST
 * Run:    /tmp/test_framebuffer_contract
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Headers under test (tests/microkit.h satisfies the <microkit.h> include)
 * ══════════════════════════════════════════════════════════════════════════ */

#include "contracts/framebuffer_contract.h"

/* ══════════════════════════════════════════════════════════════════════════
 * Test infrastructure
 * ══════════════════════════════════════════════════════════════════════════ */

#define PASS(name)  do { printf("  PASS  %s\n", name); return 0; } while(0)
#define FAIL(msg)   do { printf("  FAIL  %s:%d: %s\n", __FILE__, __LINE__, msg); return 1; } while(0)
#define CHECK(cond) do { if (!(cond)) FAIL(#cond); } while(0)

/* ══════════════════════════════════════════════════════════════════════════
 * Opcode range and value tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_fb_opcodes(void)
{
    /* All MSG_FB_* must be in the 0x2500 range */
    CHECK((MSG_FB_CREATE       & 0xFF00u) == 0x2500u);
    CHECK((MSG_FB_WRITE        & 0xFF00u) == 0x2500u);
    CHECK((MSG_FB_FLIP         & 0xFF00u) == 0x2500u);
    CHECK((MSG_FB_RESIZE       & 0xFF00u) == 0x2500u);
    CHECK((MSG_FB_DESTROY      & 0xFF00u) == 0x2500u);
    CHECK((MSG_FB_FRAME_READY  & 0xFF00u) == 0x2500u);

    /* Exact values per agentos.h */
    CHECK(MSG_FB_CREATE      == 0x2501u);
    CHECK(MSG_FB_WRITE       == 0x2502u);
    CHECK(MSG_FB_FLIP        == 0x2503u);
    CHECK(MSG_FB_RESIZE      == 0x2504u);
    CHECK(MSG_FB_DESTROY     == 0x2505u);
    CHECK(MSG_FB_FRAME_READY == 0x2506u);

    /* Opcodes must be unique */
    CHECK(MSG_FB_CREATE  != MSG_FB_WRITE);
    CHECK(MSG_FB_WRITE   != MSG_FB_FLIP);
    CHECK(MSG_FB_FLIP    != MSG_FB_RESIZE);
    CHECK(MSG_FB_RESIZE  != MSG_FB_DESTROY);
    CHECK(MSG_FB_DESTROY != MSG_FB_FRAME_READY);

    PASS("test_fb_opcodes");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Backend selection constants
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_fb_backend_constants(void)
{
    CHECK(FB_BACKEND_NULL       == 0u);
    CHECK(FB_BACKEND_HW_DIRECT  == 1u);
    CHECK(FB_BACKEND_REMOTE_API == 2u);

    /* Backends must be distinct */
    CHECK(FB_BACKEND_NULL       != FB_BACKEND_HW_DIRECT);
    CHECK(FB_BACKEND_HW_DIRECT  != FB_BACKEND_REMOTE_API);
    CHECK(FB_BACKEND_NULL       != FB_BACKEND_REMOTE_API);

    PASS("test_fb_backend_constants");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Pixel format constants
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_fb_pixel_formats(void)
{
    /* All formats must be distinct */
    CHECK(FB_FMT_XRGB8888 != FB_FMT_ARGB8888);
    CHECK(FB_FMT_ARGB8888 != FB_FMT_RGB565);
    CHECK(FB_FMT_XRGB8888 != FB_FMT_RGB565);

    /* Exact values */
    CHECK(FB_FMT_XRGB8888 == 0x00u);
    CHECK(FB_FMT_ARGB8888 == 0x01u);
    CHECK(FB_FMT_RGB565   == 0x02u);

    PASS("test_fb_pixel_formats");
}

/* ══════════════════════════════════════════════════════════════════════════
 * MSG_FB_CREATE struct
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_fb_create_structs(void)
{
    struct fb_create_req req;
    memset(&req, 0, sizeof(req));
    req.width   = 1920u;
    req.height  = 1080u;
    req.format  = FB_FMT_XRGB8888;
    req.backend = FB_BACKEND_NULL;

    CHECK(req.width   == 1920u);
    CHECK(req.height  == 1080u);
    CHECK(req.format  == FB_FMT_XRGB8888);
    CHECK(req.backend == FB_BACKEND_NULL);

    struct fb_create_reply reply;
    memset(&reply, 0, sizeof(reply));
    reply.ok     = (uint32_t)FB_OK;
    reply.handle = 0u;

    CHECK(reply.ok     == (uint32_t)FB_OK);
    CHECK(reply.handle == 0u);

    /* Error sentinel: handle 0xFFFFFFFF indicates failure */
    reply.handle = 0xFFFFFFFFu;
    CHECK(reply.handle == 0xFFFFFFFFu);

    PASS("test_fb_create_structs");
}

/* ══════════════════════════════════════════════════════════════════════════
 * MSG_FB_WRITE struct
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_fb_write_structs(void)
{
    struct fb_write_req req;
    memset(&req, 0, sizeof(req));
    req.x            = 0u;
    req.y            = 0u;
    req.w            = 1920u;
    req.h            = 1080u;
    req.shmem_offset = 0u;
    req.stride       = 1920u * 4u;  /* XRGB8888: 4 bytes per pixel */

    CHECK(req.w      == 1920u);
    CHECK(req.h      == 1080u);
    CHECK(req.stride == 7680u);

    /* Partial blit: top-left 100×100 region */
    struct fb_write_req partial;
    memset(&partial, 0, sizeof(partial));
    partial.x = 0u;
    partial.y = 0u;
    partial.w = 100u;
    partial.h = 100u;
    CHECK(partial.x + partial.w <= 1920u);
    CHECK(partial.y + partial.h <= 1080u);

    struct fb_write_reply reply;
    memset(&reply, 0, sizeof(reply));
    reply.ok = (uint32_t)FB_OK;
    CHECK(reply.ok == (uint32_t)FB_OK);

    PASS("test_fb_write_structs");
}

/* ══════════════════════════════════════════════════════════════════════════
 * MSG_FB_FLIP struct
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_fb_flip_structs(void)
{
    struct fb_flip_reply reply;
    memset(&reply, 0, sizeof(reply));
    reply.ok        = (uint32_t)FB_OK;
    reply.frame_seq = 1u;

    CHECK(reply.ok        == (uint32_t)FB_OK);
    CHECK(reply.frame_seq == 1u);

    /* frame_seq must be monotonically increasing */
    uint32_t prev = reply.frame_seq;
    reply.frame_seq = 2u;
    CHECK(reply.frame_seq > prev);

    PASS("test_fb_flip_structs");
}

/* ══════════════════════════════════════════════════════════════════════════
 * MSG_FB_RESIZE struct
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_fb_resize_structs(void)
{
    struct fb_resize_req req;
    memset(&req, 0, sizeof(req));
    req.new_width  = 3840u;
    req.new_height = 2160u;

    CHECK(req.new_width  == 3840u);
    CHECK(req.new_height == 2160u);
    CHECK(req.new_width  != 0u);  /* invariant: zero dimensions are rejected */
    CHECK(req.new_height != 0u);

    struct fb_resize_reply reply;
    memset(&reply, 0, sizeof(reply));
    reply.ok = (uint32_t)FB_OK;
    CHECK(reply.ok == (uint32_t)FB_OK);

    PASS("test_fb_resize_structs");
}

/* ══════════════════════════════════════════════════════════════════════════
 * MSG_FB_DESTROY struct
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_fb_destroy_structs(void)
{
    struct fb_destroy_reply reply;
    memset(&reply, 0, sizeof(reply));
    reply.ok = (uint32_t)FB_OK;
    CHECK(reply.ok == (uint32_t)FB_OK);

    PASS("test_fb_destroy_structs");
}

/* ══════════════════════════════════════════════════════════════════════════
 * fb_frame_ready_event_t layout
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_fb_frame_ready_event(void)
{
    fb_frame_ready_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.handle    = 0u;
    ev.frame_seq = 42u;
    ev.backend   = FB_BACKEND_NULL;

    CHECK(ev.handle    == 0u);
    CHECK(ev.frame_seq == 42u);
    CHECK(ev.backend   == FB_BACKEND_NULL);

    /* Packed: 4 × uint32_t = 16 bytes */
    CHECK(sizeof(fb_frame_ready_event_t) == 4u * sizeof(uint32_t));

    /* EventBus event ID must be distinct from MSG_FB_* opcodes */
    CHECK(EVENT_FB_FRAME_READY != MSG_FB_CREATE);
    CHECK(EVENT_FB_FRAME_READY != MSG_FB_FLIP);
    CHECK(EVENT_FB_FRAME_READY == 0x40u);

    PASS("test_fb_frame_ready_event");
}

/* ══════════════════════════════════════════════════════════════════════════
 * fb_error_t enum completeness
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_fb_error_codes(void)
{
    CHECK(FB_OK                  == 0);
    CHECK(FB_ERR_BAD_HANDLE      == 1);
    CHECK(FB_ERR_NO_SLOTS        == 2);
    CHECK(FB_ERR_BAD_GEOMETRY    == 3);
    CHECK(FB_ERR_BAD_FORMAT      == 4);
    CHECK(FB_ERR_BAD_BACKEND     == 5);
    CHECK(FB_ERR_OUT_OF_BOUNDS   == 6);
    CHECK(FB_ERR_BACKEND_UNAVAIL == 7);

    /* FB_OK must be zero so error checks like `if (err)` work correctly */
    CHECK(FB_OK == 0);

    /* All error codes must be distinct */
    CHECK(FB_ERR_BAD_HANDLE  != FB_ERR_NO_SLOTS);
    CHECK(FB_ERR_BAD_GEOMETRY!= FB_ERR_BAD_FORMAT);
    CHECK(FB_ERR_BAD_FORMAT  != FB_ERR_BAD_BACKEND);
    CHECK(FB_ERR_BAD_BACKEND != FB_ERR_OUT_OF_BOUNDS);
    CHECK(FB_ERR_OUT_OF_BOUNDS != FB_ERR_BACKEND_UNAVAIL);

    PASS("test_fb_error_codes");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Channel ID test
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_fb_channel_id(void)
{
    /* FB_PD_CH_CONTROLLER must be CH_FB_PD (71) */
    CHECK(FB_PD_CH_CONTROLLER == CH_FB_PD);
    CHECK(FB_PD_CH_CONTROLLER == 71u);

    PASS("test_fb_channel_id");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Protocol ordering invariant test
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_fb_protocol_invariants(void)
{
    /*
     * Simulate the required protocol using state machine:
     * CREATE → WRITE(s) → FLIP → RESIZE (optional) → DESTROY
     */

    /* Step 1: CREATE succeeds and returns a handle */
    struct fb_create_req create_req;
    memset(&create_req, 0, sizeof(create_req));
    create_req.width   = 800u;
    create_req.height  = 600u;
    create_req.format  = FB_FMT_RGB565;
    create_req.backend = FB_BACKEND_NULL;

    CHECK(create_req.width  != 0u);   /* width must be non-zero */
    CHECK(create_req.height != 0u);   /* height must be non-zero */

    uint32_t handle = 0u;  /* simulated handle from CREATE */

    /* Step 2: WRITE with a dirty rect that fits within the framebuffer */
    struct fb_write_req write_req;
    memset(&write_req, 0, sizeof(write_req));
    write_req.x = 0u;
    write_req.y = 0u;
    write_req.w = 100u;
    write_req.h = 100u;
    write_req.shmem_offset = 0u;
    write_req.stride       = 100u * 2u;  /* RGB565: 2 bytes per pixel */

    /* dirty rect must fit within framebuffer bounds */
    CHECK(write_req.x + write_req.w <= create_req.width);
    CHECK(write_req.y + write_req.h <= create_req.height);

    /* Step 3: FLIP commits the frame; frame_seq is returned */
    uint32_t frame_seq = 1u;
    CHECK(frame_seq > 0u);

    /* Step 4: FLIP again increments frame_seq */
    uint32_t frame_seq2 = frame_seq + 1u;
    CHECK(frame_seq2 > frame_seq);

    /* Step 5: RESIZE to new dimensions */
    struct fb_resize_req resize_req;
    memset(&resize_req, 0, sizeof(resize_req));
    resize_req.new_width  = 1024u;
    resize_req.new_height = 768u;
    CHECK(resize_req.new_width  > create_req.width);  /* resize to larger */
    CHECK(resize_req.new_height > create_req.height);

    /* Step 6: DESTROY releases the handle */
    struct fb_destroy_reply destroy_reply = { .ok = (uint32_t)FB_OK };
    CHECK(destroy_reply.ok == (uint32_t)FB_OK);

    (void)handle;
    PASS("test_fb_protocol_invariants");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Guest binding integration test (uses guest_contract.h constants)
 * ══════════════════════════════════════════════════════════════════════════ */

#include "contracts/guest_contract.h"

static int test_fb_guest_binding(void)
{
    /*
     * MSG_GUEST_BIND_DEVICE with GUEST_DEV_FB must accept a framebuffer handle
     * as dev_handle and return a cap_token stored in guest_capabilities_t.fb_token.
     */
    struct guest_bind_device_req bind_req;
    memset(&bind_req, 0, sizeof(bind_req));
    bind_req.guest_id   = 0u;
    bind_req.dev_type   = GUEST_DEV_FB;
    bind_req.dev_handle = 0u;  /* handle from MSG_FB_CREATE */

    CHECK(bind_req.dev_type == GUEST_DEV_FB);
    CHECK(bind_req.dev_type == 4u);   /* GUEST_DEV_FB ordinal */

    /* fb_token slot must exist in guest_capabilities_t */
    guest_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.fb_token = 0xBEEFu;
    CHECK(caps.fb_token == 0xBEEFu);
    CHECK(caps.fb_token != GUEST_CAP_TOKEN_INVALID);

    /* GUEST_DEV_FLAG_FB must be the correct bit */
    CHECK(GUEST_DEV_FLAG_FB == (1u << GUEST_DEV_FB));
    CHECK(GUEST_DEV_FLAG_FB == (1u << 4));

    PASS("test_fb_guest_binding");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("[framebuffer_contract] contract unit tests\n");

    int failures = 0;
    failures += test_fb_opcodes();
    failures += test_fb_backend_constants();
    failures += test_fb_pixel_formats();
    failures += test_fb_create_structs();
    failures += test_fb_write_structs();
    failures += test_fb_flip_structs();
    failures += test_fb_resize_structs();
    failures += test_fb_destroy_structs();
    failures += test_fb_frame_ready_event();
    failures += test_fb_error_codes();
    failures += test_fb_channel_id();
    failures += test_fb_protocol_invariants();
    failures += test_fb_guest_binding();

    if (failures == 0)
        printf("\n[framebuffer_contract] ALL TESTS PASSED\n");
    else
        printf("\n[framebuffer_contract] %d TEST(S) FAILED\n", failures);

    return failures ? 1 : 0;
}
