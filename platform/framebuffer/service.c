#include <platform/framebuffer.h>
#include <string.h>

static uint32_t load(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void publish(uint32_t *p, uint32_t value)
{
    __atomic_store_n(p, value, __ATOMIC_RELEASE);
}

int aos_fb_submit(aos_fb_region_t *r, const aos_fb_request_t *request)
{
    if (!r || !request) return -1;
    uint32_t tail = load(&r->req_tail), head = load(&r->req_head);
    if (tail - head >= AOS_FB_QUEUE_CAPACITY) return -1;
    r->requests[tail % AOS_FB_QUEUE_CAPACITY] = *request;
    publish(&r->req_tail, tail + 1u);
    return 0;
}

int aos_fb_receive(aos_fb_region_t *r, aos_fb_response_t *response)
{
    if (!r || !response) return -1;
    uint32_t tail = load(&r->resp_tail), head = load(&r->resp_head);
    if (tail == head || tail - head > AOS_FB_QUEUE_CAPACITY) return -1;
    *response = r->responses[head % AOS_FB_QUEUE_CAPACITY];
    publish(&r->resp_head, head + 1u);
    return 0;
}

int aos_fb_client_init(aos_fb_client_t *c, aos_fb_region_t *r,
                       uint8_t *arena, size_t bytes)
{
    if (!c || !r || !arena || bytes < AOS_FB_ARENA_BYTES) return -1;
    memset(c, 0, sizeof(*c));
    c->region = r;
    c->next_handle = 1;
    for (unsigned i = 0; i < AOS_FB_MAX_SURFACES; ++i) {
        c->surfaces[i].staging = arena + (2u * i) * AOS_FB_SURFACE_BYTES;
        c->surfaces[i].committed = arena + (2u * i + 1u) * AOS_FB_SURFACE_BYTES;
    }
    return 0;
}

static aos_fb_surface_t *lookup(aos_fb_client_t *c, uint64_t handle)
{
    if (handle == 0) return NULL;
    for (unsigned i = 0; i < AOS_FB_MAX_SURFACES; ++i)
        if (c->surfaces[i].handle == handle) return &c->surfaces[i];
    return NULL;
}

static int rectangle_valid(const aos_fb_surface_t *s, const aos_fb_request_t *q)
{
    if (!q->width || !q->height || q->x > s->width || q->y > s->height ||
        q->width > s->width - q->x || q->height > s->height - q->y)
        return 0;
    /* Dimensions are now bounded by the privately retained surface size. */
    uint32_t bytes = q->width * q->height * AOS_FB_PIXEL_BYTES;
    return q->data_offset <= AOS_FB_DATA_BYTES &&
           q->data_length == bytes && bytes <= AOS_FB_DATA_BYTES - q->data_offset;
}

static aos_fb_response_t execute(aos_fb_client_t *c, const aos_fb_request_t *q)
{
    aos_fb_response_t p = { .version = AOS_FB_VERSION, .id = q->id,
                            .handle = q->handle };
    if (q->version != AOS_FB_VERSION) { p.status = AOS_FB_BAD_VERSION; return p; }
    if (q->reserved) { p.status = AOS_FB_BAD_OPERATION; return p; }
    if (q->operation < AOS_FB_CREATE || q->operation > AOS_FB_DESTROY) {
        p.status = AOS_FB_BAD_OPERATION;
        return p;
    }
    aos_fb_surface_t *s = NULL;
    if (q->operation == AOS_FB_CREATE) {
        if (!q->width || !q->height || q->width > AOS_FB_MAX_WIDTH ||
            q->height > AOS_FB_MAX_HEIGHT) { p.status = AOS_FB_BAD_BOUNDS; return p; }
        for (unsigned i = 0; i < AOS_FB_MAX_SURFACES; ++i)
            if (!c->surfaces[i].handle) { s = &c->surfaces[i]; break; }
        if (!s || !c->next_handle) { p.status = AOS_FB_NO_SPACE; return p; }
        s->handle = c->next_handle++;
        s->sequence = 0;
        s->width = q->width;
        s->height = q->height;
        size_t bytes = (size_t)s->width * s->height * AOS_FB_PIXEL_BYTES;
        memset(s->staging, 0, bytes);
        memset(s->committed, 0, bytes);
    } else {
        s = lookup(c, q->handle);
        if (!s) { p.status = AOS_FB_BAD_HANDLE; return p; }
        if (q->operation == AOS_FB_WRITE || q->operation == AOS_FB_READ) {
            if (!rectangle_valid(s, q)) { p.status = AOS_FB_BAD_BOUNDS; return p; }
            size_t row_bytes = (size_t)q->width * AOS_FB_PIXEL_BYTES;
            for (uint32_t row = 0; row < q->height; ++row) {
                size_t pixel = ((size_t)(q->y + row) * s->width + q->x) * AOS_FB_PIXEL_BYTES;
                uint8_t *data = c->region->data + q->data_offset + row * row_bytes;
                if (q->operation == AOS_FB_WRITE)
                    memcpy(s->staging + pixel, data, row_bytes);
                else
                    memcpy(data, s->committed + pixel, row_bytes);
            }
        } else if (q->operation == AOS_FB_FLIP) {
            if (s->sequence == UINT64_MAX) { p.status = AOS_FB_NO_SPACE; return p; }
            memcpy(s->committed, s->staging,
                   (size_t)s->width * s->height * AOS_FB_PIXEL_BYTES);
            ++s->sequence;
        }
    }
    p.handle = s->handle;
    p.sequence = s->sequence;
    p.width = s->width;
    p.height = s->height;
    if (q->operation == AOS_FB_DESTROY) s->handle = 0;
    return p;
}

unsigned aos_fb_pump(aos_fb_client_t *c)
{
    if (!c || !c->region) return 0;
    aos_fb_region_t *r = c->region;
    unsigned completed = 0;
    while (completed < AOS_FB_QUEUE_CAPACITY) {
        uint32_t head = load(&r->req_head), tail = load(&r->req_tail);
        uint32_t out = load(&r->resp_tail), consumed = load(&r->resp_head);
        if (head == tail || tail - head > AOS_FB_QUEUE_CAPACITY ||
            out - consumed >= AOS_FB_QUEUE_CAPACITY) break;
        /* Snapshot metadata once. All pointer arithmetic below uses this
         * bounded private copy, even if a client rewrites its shared slot. */
        aos_fb_request_t request = r->requests[head % AOS_FB_QUEUE_CAPACITY];
        aos_fb_response_t response = execute(c, &request);
        r->responses[out % AOS_FB_QUEUE_CAPACITY] = response;
        publish(&r->resp_tail, out + 1u);
        publish(&r->req_head, head + 1u);
        ++completed;
    }
    return completed;
}
