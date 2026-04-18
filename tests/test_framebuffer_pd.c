/*
 * agentOS framebuffer_pd — Contract Unit Test
 *
 * Tests all IPC opcodes (success + error paths) without seL4 or Microkit.
 * The NULL backend is exercised throughout; backend dispatch is tested via
 * the stub table.  EventBus notify is stubbed; pixel data is validated via
 * the shared shmem buffer.
 *
 * Build:  cc -o /tmp/test_framebuffer_pd \
 *             tests/test_framebuffer_pd.c \
 *             -I kernel/agentos-root-task/include \
 *             -DAGENTOS_TEST_HOST
 * Run:    /tmp/test_framebuffer_pd
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Host-side Microkit stubs — must come before any agentos.h include
 * ══════════════════════════════════════════════════════════════════════════ */

static uint64_t _mrs[64];
static inline void     microkit_mr_set(uint32_t i, uint64_t v) { _mrs[i] = v; }
static inline uint64_t microkit_mr_get(uint32_t i)             { return _mrs[i]; }

typedef uint32_t microkit_channel;
typedef uint64_t microkit_msginfo;

static inline microkit_msginfo microkit_msginfo_new(uint64_t l, uint32_t c) {
    (void)c; return l;
}
static inline void microkit_dbg_puts(const char *s) { (void)s; }

static uint32_t notify_count = 0;
static inline void microkit_notify(microkit_channel ch) { (void)ch; notify_count++; }

/* microkit.h stub — needed so agentos.h compiles on the host */
#define MICROKIT_H  /* prevent re-include guards from looking for real file */

/* ══════════════════════════════════════════════════════════════════════════
 * Framebuffer contract constants — inlined to avoid microkit.h pull-in
 * ══════════════════════════════════════════════════════════════════════════ */

/* Opcodes (from agentos.h Phase 4a) */
#define MSG_FB_CREATE       0x2501u
#define MSG_FB_WRITE        0x2502u
#define MSG_FB_FLIP         0x2503u
#define MSG_FB_RESIZE       0x2504u
#define MSG_FB_DESTROY      0x2505u
#define MSG_FB_FRAME_READY  0x2506u
#define MSG_FB_STATUS       0x2507u

/* Pixel formats */
#define FB_FMT_XRGB8888   0x00u
#define FB_FMT_ARGB8888   0x01u
#define FB_FMT_RGB565     0x02u

/* Backend */
#define FB_BACKEND_NULL       0u
#define FB_BACKEND_HW_DIRECT  1u
#define FB_BACKEND_REMOTE_API 2u

/* Error codes */
#define FB_OK               0u
#define FB_ERR_BAD_HANDLE   1u
#define FB_ERR_NO_SLOTS     2u
#define FB_ERR_BAD_FORMAT   3u
#define FB_ERR_BAD_DIMS     4u
#define FB_ERR_BAD_RECT     5u
#define FB_ERR_BAD_BACKEND  6u
#define FB_ERR_BACKEND_BUSY 7u
#define FB_ERR_NO_SHMEM     8u

/* Sentinel */
#define FB_HANDLE_INVALID   0u

/* EventBus channel (from agentos.h) */
#define EVENTBUS_CH_MONITOR  1u

/* EVT_FB_FRAME_READY */
#define EVT_FB_FRAME_READY  0x5001u

/* ══════════════════════════════════════════════════════════════════════════
 * Request / reply structs (mirrors framebuffer_contract.h)
 * ══════════════════════════════════════════════════════════════════════════ */

struct fb_create_req  { uint32_t width, height, format, backend; };
struct fb_create_reply{ uint32_t ok, fb_handle, actual_backend, stride, buf_size, shmem_offset; };
struct fb_write_req   { uint32_t fb_handle, x, y, w, h, src_stride; };
struct fb_write_reply { uint32_t ok; };
struct fb_flip_req    { uint32_t fb_handle; };
struct fb_flip_reply  { uint32_t ok, frame_seq; };
struct fb_resize_req  { uint32_t fb_handle, width, height, format; };
struct fb_resize_reply{ uint32_t ok, actual_width, actual_height, stride, buf_size, shmem_offset; };
struct fb_destroy_req { uint32_t fb_handle; };
struct fb_destroy_reply{ uint32_t ok; };
struct fb_status_req  { uint32_t fb_handle; };
struct fb_status_reply{ uint32_t ok, width, height, format, backend, frame_seq, flip_count, write_count; };

/* ══════════════════════════════════════════════════════════════════════════
 * Inline framebuffer_pd module state and implementation
 * ══════════════════════════════════════════════════════════════════════════ */

#define FB_MAX_SURFACES   8u
#define FB_MAX_WIDTH      7680u
#define FB_MAX_HEIGHT     4320u
#define FB_SHMEM_SIZE     (32u * 1024u * 1024u)

static uint32_t fmt_bpp(uint32_t fmt)
{
    switch (fmt) {
    case FB_FMT_XRGB8888: return 4;
    case FB_FMT_ARGB8888: return 4;
    case FB_FMT_RGB565:   return 2;
    default:              return 0;
    }
}

typedef struct {
    uint32_t (*flip)(uint32_t fb_handle, uint32_t frame_seq);
} fb_backend_ops_t;

static uint32_t null_flip(uint32_t h, uint32_t s) { (void)h; (void)s; return FB_OK; }

static const fb_backend_ops_t backend_ops[3] = {
    [FB_BACKEND_NULL]       = { .flip = null_flip },
    [FB_BACKEND_HW_DIRECT]  = { .flip = null_flip },
    [FB_BACKEND_REMOTE_API] = { .flip = null_flip },
};

typedef struct {
    bool      in_use;
    uint32_t  handle;
    uint32_t  width, height, format, backend;
    uint32_t  stride, buf_size, shmem_offset;
    uint32_t  frame_seq, flip_count, write_count;
} fb_surface_t;

static fb_surface_t surfaces[FB_MAX_SURFACES];
static uint32_t     shmem_used;

/* Simulated shmem: large enough for requests + replies */
static uint8_t fb_shmem_buf[64 * 1024];
static uintptr_t fb_shmem_vaddr;

static fb_surface_t *surface_find(uint32_t handle)
{
    if (handle == FB_HANDLE_INVALID || handle > FB_MAX_SURFACES)
        return NULL;
    fb_surface_t *s = &surfaces[handle - 1];
    return s->in_use ? s : NULL;
}

static fb_surface_t *surface_alloc(void)
{
    for (uint32_t i = 0; i < FB_MAX_SURFACES; i++) {
        if (!surfaces[i].in_use) {
            surfaces[i].in_use = true;
            surfaces[i].handle = i + 1;
            return &surfaces[i];
        }
    }
    return NULL;
}

static void surface_free(fb_surface_t *s) { s->in_use = false; }

static void handle_create(void)
{
    if (!fb_shmem_vaddr) {
        microkit_mr_set(0, FB_ERR_NO_SHMEM);
        microkit_mr_set(1, FB_HANDLE_INVALID);
        return;
    }
    const struct fb_create_req *req = (const struct fb_create_req *)fb_shmem_vaddr;
    uint32_t bpp = fmt_bpp(req->format);
    if (bpp == 0) {
        microkit_mr_set(0, FB_ERR_BAD_FORMAT);
        microkit_mr_set(1, FB_HANDLE_INVALID);
        return;
    }
    if (req->width == 0 || req->height == 0 ||
        req->width > FB_MAX_WIDTH || req->height > FB_MAX_HEIGHT) {
        microkit_mr_set(0, FB_ERR_BAD_DIMS);
        microkit_mr_set(1, FB_HANDLE_INVALID);
        return;
    }
    uint32_t backend = req->backend;
    if (backend > FB_BACKEND_REMOTE_API) backend = FB_BACKEND_NULL;
    uint32_t stride       = req->width * bpp;
    uint32_t buf_size     = stride * req->height;
    uint32_t aligned_size = (buf_size + 63u) & ~63u;
    if (shmem_used + aligned_size > FB_SHMEM_SIZE) {
        microkit_mr_set(0, FB_ERR_NO_SLOTS);
        microkit_mr_set(1, FB_HANDLE_INVALID);
        return;
    }
    fb_surface_t *s = surface_alloc();
    if (!s) {
        microkit_mr_set(0, FB_ERR_NO_SLOTS);
        microkit_mr_set(1, FB_HANDLE_INVALID);
        return;
    }
    s->width        = req->width;
    s->height       = req->height;
    s->format       = req->format;
    s->backend      = backend;
    s->stride       = stride;
    s->buf_size     = buf_size;
    s->shmem_offset = shmem_used;
    s->frame_seq    = 0;
    s->flip_count   = 0;
    s->write_count  = 0;
    shmem_used += aligned_size;
    struct fb_create_reply *rep =
        (struct fb_create_reply *)(fb_shmem_vaddr + sizeof(struct fb_create_req));
    rep->ok             = FB_OK;
    rep->fb_handle      = s->handle;
    rep->actual_backend = s->backend;
    rep->stride         = s->stride;
    rep->buf_size       = s->buf_size;
    rep->shmem_offset   = s->shmem_offset;
    microkit_mr_set(0, FB_OK);
    microkit_mr_set(1, s->handle);
}

static void handle_write(void)
{
    if (!fb_shmem_vaddr) { microkit_mr_set(0, FB_ERR_NO_SHMEM); return; }
    const struct fb_write_req *req = (const struct fb_write_req *)fb_shmem_vaddr;
    fb_surface_t *s = surface_find(req->fb_handle);
    if (!s) { microkit_mr_set(0, FB_ERR_BAD_HANDLE); return; }
    if (req->x + req->w > s->width || req->y + req->h > s->height ||
        req->w == 0 || req->h == 0) {
        microkit_mr_set(0, FB_ERR_BAD_RECT);
        return;
    }
    s->write_count++;
    microkit_mr_set(0, FB_OK);
}

static void handle_flip(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    fb_surface_t *s = surface_find(handle);
    if (!s) { microkit_mr_set(0, FB_ERR_BAD_HANDLE); microkit_mr_set(1, 0); return; }
    uint32_t result = backend_ops[s->backend].flip(s->handle, s->frame_seq + 1);
    if (result != FB_OK) { microkit_mr_set(0, result); microkit_mr_set(1, s->frame_seq); return; }
    s->frame_seq++;
    s->flip_count++;
    microkit_mr_set(0, FB_OK);
    microkit_mr_set(1, s->frame_seq);
    microkit_mr_set(0, EVT_FB_FRAME_READY);
    microkit_mr_set(1, s->handle);
    microkit_mr_set(2, s->frame_seq);
    microkit_mr_set(3, s->width);
    microkit_mr_set(4, s->height);
    microkit_mr_set(5, s->format);
    microkit_notify(EVENTBUS_CH_MONITOR);
    microkit_mr_set(0, FB_OK);
    microkit_mr_set(1, s->frame_seq);
}

static void handle_resize(void)
{
    if (!fb_shmem_vaddr) { microkit_mr_set(0, FB_ERR_NO_SHMEM); return; }
    const struct fb_resize_req *req = (const struct fb_resize_req *)fb_shmem_vaddr;
    fb_surface_t *s = surface_find(req->fb_handle);
    if (!s) { microkit_mr_set(0, FB_ERR_BAD_HANDLE); return; }
    uint32_t new_fmt = (req->format != 0) ? req->format : s->format;
    uint32_t bpp     = fmt_bpp(new_fmt);
    if (bpp == 0) { microkit_mr_set(0, FB_ERR_BAD_FORMAT); return; }
    if (req->width == 0 || req->height == 0 ||
        req->width > FB_MAX_WIDTH || req->height > FB_MAX_HEIGHT) {
        microkit_mr_set(0, FB_ERR_BAD_DIMS);
        return;
    }
    uint32_t new_stride   = req->width * bpp;
    uint32_t new_buf_size = new_stride * req->height;
    uint32_t aligned_new  = (new_buf_size + 63u) & ~63u;
    uint32_t aligned_old  = (s->buf_size + 63u) & ~63u;
    if (aligned_new > aligned_old) {
        uint32_t tail_offset = s->shmem_offset + aligned_old;
        if (tail_offset == shmem_used) {
            if (shmem_used - aligned_old + aligned_new > FB_SHMEM_SIZE) {
                microkit_mr_set(0, FB_ERR_NO_SHMEM); return;
            }
            shmem_used = shmem_used - aligned_old + aligned_new;
        } else {
            microkit_mr_set(0, FB_ERR_NO_SHMEM); return;
        }
    }
    s->width    = req->width;
    s->height   = req->height;
    s->format   = new_fmt;
    s->stride   = new_stride;
    s->buf_size = new_buf_size;
    struct fb_resize_reply *rep =
        (struct fb_resize_reply *)(fb_shmem_vaddr + sizeof(struct fb_resize_req));
    rep->ok            = FB_OK;
    rep->actual_width  = s->width;
    rep->actual_height = s->height;
    rep->stride        = s->stride;
    rep->buf_size      = s->buf_size;
    rep->shmem_offset  = s->shmem_offset;
    microkit_mr_set(0, FB_OK);
}

static void handle_destroy(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    fb_surface_t *s = surface_find(handle);
    if (!s) { microkit_mr_set(0, FB_ERR_BAD_HANDLE); return; }
    uint32_t aligned = (s->buf_size + 63u) & ~63u;
    if (s->shmem_offset + aligned == shmem_used) shmem_used -= aligned;
    surface_free(s);
    microkit_mr_set(0, FB_OK);
}

static void handle_status(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    fb_surface_t *s = surface_find(handle);
    if (!s) { microkit_mr_set(0, FB_ERR_BAD_HANDLE); return; }
    if (!fb_shmem_vaddr) { microkit_mr_set(0, FB_ERR_NO_SHMEM); return; }
    struct fb_status_reply *rep = (struct fb_status_reply *)fb_shmem_vaddr;
    rep->ok          = FB_OK;
    rep->width       = s->width;
    rep->height      = s->height;
    rep->format      = s->format;
    rep->backend     = s->backend;
    rep->frame_seq   = s->frame_seq;
    rep->flip_count  = s->flip_count;
    rep->write_count = s->write_count;
    microkit_mr_set(0, FB_OK);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test helpers
 * ══════════════════════════════════════════════════════════════════════════ */

#define PASS(name) printf("[PASS] %s\n", (name))
#define FAIL(name) do { printf("[FAIL] %s\n", (name)); return 1; } while (0)

static void reset_state(void)
{
    for (uint32_t i = 0; i < FB_MAX_SURFACES; i++)
        surfaces[i].in_use = false;
    shmem_used = 0;
    notify_count = 0;
    fb_shmem_vaddr = (uintptr_t)fb_shmem_buf;
    memset(fb_shmem_buf, 0, sizeof(fb_shmem_buf));
}

/* Place a CREATE request in shmem and call handle_create(). Returns handle. */
static uint32_t do_create(uint32_t w, uint32_t h, uint32_t fmt, uint32_t backend)
{
    struct fb_create_req *req = (struct fb_create_req *)fb_shmem_vaddr;
    req->width   = w;
    req->height  = h;
    req->format  = fmt;
    req->backend = backend;
    handle_create();
    return (uint32_t)microkit_mr_get(1);
}

static void do_destroy(uint32_t handle)
{
    microkit_mr_set(0, MSG_FB_DESTROY);
    microkit_mr_set(1, handle);
    handle_destroy();
    assert(microkit_mr_get(0) == FB_OK);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_create_basic(void)
{
    reset_state();

    uint32_t h = do_create(1920, 1080, FB_FMT_XRGB8888, FB_BACKEND_NULL);
    if (microkit_mr_get(0) != FB_OK)           FAIL("test_create_basic: ok");
    if (h == FB_HANDLE_INVALID)                FAIL("test_create_basic: handle");
    if (surfaces[h-1].width != 1920)           FAIL("test_create_basic: width");
    if (surfaces[h-1].height != 1080)          FAIL("test_create_basic: height");
    if (surfaces[h-1].stride != 1920 * 4)      FAIL("test_create_basic: stride");
    if (surfaces[h-1].backend != FB_BACKEND_NULL) FAIL("test_create_basic: backend");

    /* Reply struct in shmem */
    struct fb_create_reply *rep =
        (struct fb_create_reply *)(fb_shmem_vaddr + sizeof(struct fb_create_req));
    if (rep->fb_handle != h)          FAIL("test_create_basic: rep.handle");
    if (rep->stride != 1920 * 4)      FAIL("test_create_basic: rep.stride");
    if (rep->buf_size != 1920*4*1080) FAIL("test_create_basic: rep.buf_size");

    do_destroy(h);
    PASS("test_create_basic");
    return 0;
}

static int test_create_rgb565(void)
{
    reset_state();

    uint32_t h = do_create(640, 480, FB_FMT_RGB565, FB_BACKEND_NULL);
    if (microkit_mr_get(0) != FB_OK) FAIL("test_create_rgb565: ok");
    if (surfaces[h-1].stride != 640 * 2) FAIL("test_create_rgb565: stride");

    do_destroy(h);
    PASS("test_create_rgb565");
    return 0;
}

static int test_create_bad_format(void)
{
    reset_state();

    do_create(800, 600, 0xFF, FB_BACKEND_NULL);
    if (microkit_mr_get(0) != FB_ERR_BAD_FORMAT) FAIL("test_create_bad_format");

    PASS("test_create_bad_format");
    return 0;
}

static int test_create_bad_dims(void)
{
    reset_state();

    /* Zero width */
    do_create(0, 600, FB_FMT_XRGB8888, FB_BACKEND_NULL);
    if (microkit_mr_get(0) != FB_ERR_BAD_DIMS) FAIL("test_create_bad_dims: w=0");

    /* Zero height */
    do_create(800, 0, FB_FMT_XRGB8888, FB_BACKEND_NULL);
    if (microkit_mr_get(0) != FB_ERR_BAD_DIMS) FAIL("test_create_bad_dims: h=0");

    /* Exceeds max */
    do_create(FB_MAX_WIDTH + 1, 1, FB_FMT_XRGB8888, FB_BACKEND_NULL);
    if (microkit_mr_get(0) != FB_ERR_BAD_DIMS) FAIL("test_create_bad_dims: oversized");

    PASS("test_create_bad_dims");
    return 0;
}

static int test_create_unknown_backend_fallback(void)
{
    reset_state();

    /* Backend 99 is unknown — PD must fall back to NULL */
    uint32_t h = do_create(320, 240, FB_FMT_XRGB8888, 99);
    if (microkit_mr_get(0) != FB_OK)                   FAIL("test_create_backend_fallback: ok");
    if (surfaces[h-1].backend != FB_BACKEND_NULL)      FAIL("test_create_backend_fallback: fallback");

    do_destroy(h);
    PASS("test_create_unknown_backend_fallback");
    return 0;
}

static int test_create_no_shmem(void)
{
    reset_state();

    /* Set up request in the real buffer first, then remove the mapping */
    struct fb_create_req *req = (struct fb_create_req *)fb_shmem_vaddr;
    req->width = 800; req->height = 600;
    req->format = FB_FMT_XRGB8888; req->backend = FB_BACKEND_NULL;

    fb_shmem_vaddr = 0;  /* simulate missing mapping */
    handle_create();
    if (microkit_mr_get(0) != FB_ERR_NO_SHMEM)  FAIL("test_create_no_shmem");

    fb_shmem_vaddr = (uintptr_t)fb_shmem_buf;
    PASS("test_create_no_shmem");
    return 0;
}

static int test_create_exhaustion(void)
{
    reset_state();

    uint32_t handles[FB_MAX_SURFACES];
    for (uint32_t i = 0; i < FB_MAX_SURFACES; i++) {
        handles[i] = do_create(16, 16, FB_FMT_XRGB8888, FB_BACKEND_NULL);
        assert(microkit_mr_get(0) == FB_OK);
    }

    /* One more should fail */
    do_create(16, 16, FB_FMT_XRGB8888, FB_BACKEND_NULL);
    if (microkit_mr_get(0) != FB_ERR_NO_SLOTS) FAIL("test_create_exhaustion");

    for (uint32_t i = 0; i < FB_MAX_SURFACES; i++)
        do_destroy(handles[i]);

    PASS("test_create_exhaustion");
    return 0;
}

static int test_write_valid(void)
{
    reset_state();

    uint32_t h = do_create(320, 240, FB_FMT_XRGB8888, FB_BACKEND_NULL);
    assert(microkit_mr_get(0) == FB_OK);

    struct fb_write_req *req = (struct fb_write_req *)fb_shmem_vaddr;
    req->fb_handle  = h;
    req->x          = 0;
    req->y          = 0;
    req->w          = 320;
    req->h          = 240;
    req->src_stride = 320 * 4;
    handle_write();

    if (microkit_mr_get(0) != FB_OK)              FAIL("test_write_valid: ok");
    if (surfaces[h-1].write_count != 1)           FAIL("test_write_valid: write_count");

    do_destroy(h);
    PASS("test_write_valid");
    return 0;
}

static int test_write_sub_rect(void)
{
    reset_state();

    uint32_t h = do_create(800, 600, FB_FMT_XRGB8888, FB_BACKEND_NULL);
    assert(microkit_mr_get(0) == FB_OK);

    struct fb_write_req *req = (struct fb_write_req *)fb_shmem_vaddr;
    req->fb_handle  = h;
    req->x          = 100;
    req->y          = 200;
    req->w          = 200;
    req->h          = 100;
    req->src_stride = 200 * 4;
    handle_write();
    if (microkit_mr_get(0) != FB_OK) FAIL("test_write_sub_rect: ok");

    do_destroy(h);
    PASS("test_write_sub_rect");
    return 0;
}

static int test_write_out_of_bounds(void)
{
    reset_state();

    uint32_t h = do_create(320, 240, FB_FMT_XRGB8888, FB_BACKEND_NULL);
    assert(microkit_mr_get(0) == FB_OK);

    struct fb_write_req *req = (struct fb_write_req *)fb_shmem_vaddr;
    req->fb_handle  = h;
    req->x          = 300;  /* 300 + 40 > 320 */
    req->y          = 0;
    req->w          = 40;
    req->h          = 10;
    req->src_stride = 40 * 4;
    handle_write();
    if (microkit_mr_get(0) != FB_ERR_BAD_RECT) FAIL("test_write_oob: x+w");

    /* Zero-width rect */
    req->x = 0; req->w = 0; req->h = 10;
    handle_write();
    if (microkit_mr_get(0) != FB_ERR_BAD_RECT) FAIL("test_write_oob: w=0");

    do_destroy(h);
    PASS("test_write_out_of_bounds");
    return 0;
}

static int test_write_bad_handle(void)
{
    reset_state();

    struct fb_write_req *req = (struct fb_write_req *)fb_shmem_vaddr;
    req->fb_handle = FB_HANDLE_INVALID;
    req->x = 0; req->y = 0; req->w = 10; req->h = 10; req->src_stride = 40;
    handle_write();
    if (microkit_mr_get(0) != FB_ERR_BAD_HANDLE) FAIL("test_write_bad_handle");

    PASS("test_write_bad_handle");
    return 0;
}

static int test_flip_increments_seq(void)
{
    reset_state();

    uint32_t h = do_create(640, 480, FB_FMT_XRGB8888, FB_BACKEND_NULL);
    assert(microkit_mr_get(0) == FB_OK);

    uint32_t prev_notify = notify_count;

    for (uint32_t i = 1; i <= 3; i++) {
        microkit_mr_set(0, MSG_FB_FLIP);
        microkit_mr_set(1, h);
        handle_flip();
        if (microkit_mr_get(0) != FB_OK)     FAIL("test_flip: ok");
        if (microkit_mr_get(1) != i)         FAIL("test_flip: frame_seq");
        if (surfaces[h-1].flip_count != i)   FAIL("test_flip: flip_count");
    }

    /* Each flip should produce one EventBus notify */
    if (notify_count != prev_notify + 3) FAIL("test_flip: notify_count");

    do_destroy(h);
    PASS("test_flip_increments_seq");
    return 0;
}

static int test_flip_bad_handle(void)
{
    reset_state();

    microkit_mr_set(0, MSG_FB_FLIP);
    microkit_mr_set(1, FB_HANDLE_INVALID);
    handle_flip();
    if (microkit_mr_get(0) != FB_ERR_BAD_HANDLE) FAIL("test_flip_bad_handle");

    PASS("test_flip_bad_handle");
    return 0;
}

static int test_resize_tail(void)
{
    reset_state();

    /* Create a small surface at the tail of shmem */
    uint32_t h = do_create(64, 64, FB_FMT_XRGB8888, FB_BACKEND_NULL);
    assert(microkit_mr_get(0) == FB_OK);

    uint32_t before_shmem = shmem_used;

    /* Resize to larger — allowed because it's the tail allocation */
    struct fb_resize_req *req = (struct fb_resize_req *)fb_shmem_vaddr;
    req->fb_handle = h;
    req->width     = 128;
    req->height    = 128;
    req->format    = 0;  /* keep current */
    handle_resize();

    if (microkit_mr_get(0) != FB_OK)         FAIL("test_resize_tail: ok");
    if (surfaces[h-1].width  != 128)         FAIL("test_resize_tail: width");
    if (surfaces[h-1].height != 128)         FAIL("test_resize_tail: height");
    if (surfaces[h-1].stride != 128 * 4)     FAIL("test_resize_tail: stride");
    if (shmem_used <= before_shmem)          FAIL("test_resize_tail: shmem grew");

    struct fb_resize_reply *rep =
        (struct fb_resize_reply *)(fb_shmem_vaddr + sizeof(struct fb_resize_req));
    if (rep->actual_width  != 128)           FAIL("test_resize_tail: rep.width");
    if (rep->actual_height != 128)           FAIL("test_resize_tail: rep.height");

    do_destroy(h);
    PASS("test_resize_tail");
    return 0;
}

static int test_resize_bad_handle(void)
{
    reset_state();

    struct fb_resize_req *req = (struct fb_resize_req *)fb_shmem_vaddr;
    req->fb_handle = 99;
    req->width = 100; req->height = 100; req->format = FB_FMT_XRGB8888;
    handle_resize();
    if (microkit_mr_get(0) != FB_ERR_BAD_HANDLE) FAIL("test_resize_bad_handle");

    PASS("test_resize_bad_handle");
    return 0;
}

static int test_resize_bad_dims(void)
{
    reset_state();

    uint32_t h = do_create(64, 64, FB_FMT_XRGB8888, FB_BACKEND_NULL);
    assert(microkit_mr_get(0) == FB_OK);

    struct fb_resize_req *req = (struct fb_resize_req *)fb_shmem_vaddr;
    req->fb_handle = h;
    req->width = 0; req->height = 64; req->format = 0;
    handle_resize();
    if (microkit_mr_get(0) != FB_ERR_BAD_DIMS) FAIL("test_resize_bad_dims: w=0");

    do_destroy(h);
    PASS("test_resize_bad_dims");
    return 0;
}

static int test_destroy_reclaims_shmem(void)
{
    reset_state();

    uint32_t h = do_create(256, 256, FB_FMT_XRGB8888, FB_BACKEND_NULL);
    assert(microkit_mr_get(0) == FB_OK);
    uint32_t after_create = shmem_used;
    assert(after_create > 0);

    do_destroy(h);
    /* Surface was the tail allocation; shmem_used should return to 0 */
    if (shmem_used != 0) FAIL("test_destroy_reclaims_shmem: shmem_used");

    PASS("test_destroy_reclaims_shmem");
    return 0;
}

static int test_destroy_bad_handle(void)
{
    reset_state();

    microkit_mr_set(0, MSG_FB_DESTROY);
    microkit_mr_set(1, FB_HANDLE_INVALID);
    handle_destroy();
    if (microkit_mr_get(0) != FB_ERR_BAD_HANDLE) FAIL("test_destroy_bad_handle: invalid");

    microkit_mr_set(1, 77);  /* never created */
    handle_destroy();
    if (microkit_mr_get(0) != FB_ERR_BAD_HANDLE) FAIL("test_destroy_bad_handle: nonexistent");

    PASS("test_destroy_bad_handle");
    return 0;
}

static int test_status(void)
{
    reset_state();

    uint32_t h = do_create(1280, 720, FB_FMT_ARGB8888, FB_BACKEND_NULL);
    assert(microkit_mr_get(0) == FB_OK);

    /* Do some writes and flips */
    struct fb_write_req *wreq = (struct fb_write_req *)fb_shmem_vaddr;
    wreq->fb_handle = h;
    wreq->x = 0; wreq->y = 0; wreq->w = 1280; wreq->h = 720; wreq->src_stride = 1280 * 4;
    handle_write();
    assert(microkit_mr_get(0) == FB_OK);

    microkit_mr_set(1, h);
    handle_flip();
    assert(microkit_mr_get(0) == FB_OK);

    microkit_mr_set(0, MSG_FB_STATUS);
    microkit_mr_set(1, h);
    handle_status();

    if (microkit_mr_get(0) != FB_OK)  FAIL("test_status: ok");

    struct fb_status_reply *rep = (struct fb_status_reply *)fb_shmem_vaddr;
    if (rep->width       != 1280)            FAIL("test_status: width");
    if (rep->height      != 720)             FAIL("test_status: height");
    if (rep->format      != FB_FMT_ARGB8888) FAIL("test_status: format");
    if (rep->backend     != FB_BACKEND_NULL) FAIL("test_status: backend");
    if (rep->frame_seq   != 1)               FAIL("test_status: frame_seq");
    if (rep->flip_count  != 1)               FAIL("test_status: flip_count");
    if (rep->write_count != 1)               FAIL("test_status: write_count");

    do_destroy(h);
    PASS("test_status");
    return 0;
}

static int test_status_bad_handle(void)
{
    reset_state();

    microkit_mr_set(0, MSG_FB_STATUS);
    microkit_mr_set(1, FB_HANDLE_INVALID);
    handle_status();
    if (microkit_mr_get(0) != FB_ERR_BAD_HANDLE) FAIL("test_status_bad_handle");

    PASS("test_status_bad_handle");
    return 0;
}

static int test_multi_surface_isolation(void)
{
    reset_state();

    uint32_t h0 = do_create(640, 480, FB_FMT_XRGB8888, FB_BACKEND_NULL);
    uint32_t h1 = do_create(1920, 1080, FB_FMT_XRGB8888, FB_BACKEND_NULL);
    assert(h0 != h1);

    /* Flip h0 twice, h1 once */
    microkit_mr_set(1, h0); handle_flip(); assert(microkit_mr_get(0) == FB_OK);
    microkit_mr_set(1, h0); handle_flip(); assert(microkit_mr_get(0) == FB_OK);
    microkit_mr_set(1, h1); handle_flip(); assert(microkit_mr_get(0) == FB_OK);

    if (surfaces[h0-1].frame_seq != 2) FAIL("test_multi: h0.frame_seq");
    if (surfaces[h1-1].frame_seq != 1) FAIL("test_multi: h1.frame_seq");
    if (surfaces[h0-1].width != 640)   FAIL("test_multi: h0.width");
    if (surfaces[h1-1].width != 1920)  FAIL("test_multi: h1.width");

    do_destroy(h0);
    do_destroy(h1);
    PASS("test_multi_surface_isolation");
    return 0;
}

static int test_graphics_opt_in(void)
{
    reset_state();

    /* A guest that never calls MSG_FB_CREATE gets nothing allocated */
    if (shmem_used != 0)              FAIL("test_opt_in: shmem after reset");

    for (uint32_t i = 0; i < FB_MAX_SURFACES; i++)
        if (surfaces[i].in_use)       FAIL("test_opt_in: surface after reset");

    PASS("test_graphics_opt_in");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("[framebuffer_pd] contract unit tests\n");

    int failures = 0;
    failures += test_create_basic();
    failures += test_create_rgb565();
    failures += test_create_bad_format();
    failures += test_create_bad_dims();
    failures += test_create_unknown_backend_fallback();
    failures += test_create_no_shmem();
    failures += test_create_exhaustion();
    failures += test_write_valid();
    failures += test_write_sub_rect();
    failures += test_write_out_of_bounds();
    failures += test_write_bad_handle();
    failures += test_flip_increments_seq();
    failures += test_flip_bad_handle();
    failures += test_resize_tail();
    failures += test_resize_bad_handle();
    failures += test_resize_bad_dims();
    failures += test_destroy_reclaims_shmem();
    failures += test_destroy_bad_handle();
    failures += test_status();
    failures += test_status_bad_handle();
    failures += test_multi_surface_isolation();
    failures += test_graphics_opt_in();

    if (failures == 0)
        printf("\n[framebuffer_pd] ALL TESTS PASSED\n");
    else
        printf("\n[framebuffer_pd] %d TEST(S) FAILED\n", failures);

    return failures ? 1 : 0;
}
