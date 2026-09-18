#define _GNU_SOURCE
#include <platform/framebuffer.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static void terminal_detach(aos_fb_client_t *peer)
{
    aos_fb_region_t *r = mmap(NULL, AOS_FB_CLIENT_STRIDE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *arena = mmap(NULL, AOS_FB_ARENA_BYTES, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(r != MAP_FAILED && arena != MAP_FAILED);
    aos_fb_client_t client;
    assert(aos_fb_client_init(&client, r, arena, AOS_FB_ARENA_BYTES) == 0);
    client.selected_handle = client.surfaces[0].handle = 17;
    client.surfaces[0].sequence = 1;
    /* Retirement must progress without consuming malformed requests or
     * waiting for a full response ring, including a selected live surface. */
    r->req_tail = AOS_FB_QUEUE_CAPACITY + 1;
    r->resp_tail = AOS_FB_QUEUE_CAPACITY;
    r->detach.version = AOS_FB_DETACH_VERSION + 1;
    r->detach.request = 1;
    assert(aos_fb_pump(&client) == 0 && !r->detach.ack && client.region == r);
    r->detach.version = AOS_FB_DETACH_VERSION;
    r->detach.request = 2;
    assert(aos_fb_pump(&client) == 0 && !r->detach.ack && client.region == r);
    __atomic_store_n(&r->detach.request, 1u, __ATOMIC_RELEASE);
    assert(aos_fb_pump(&client) == 1);
    assert(__atomic_load_n(&r->detach.ack, __ATOMIC_ACQUIRE) == 1);
    assert(!client.region && !client.selected_handle && !client.next_handle);
    for (unsigned i = 0; i < AOS_FB_MAX_SURFACES; i++)
        assert(!client.surfaces[i].handle && !client.surfaces[i].staging &&
               !client.surfaces[i].committed);
    assert(r->req_head == 0 && r->resp_tail == AOS_FB_QUEUE_CAPACITY);
    assert(mprotect(r, AOS_FB_CLIENT_STRIDE, PROT_NONE) == 0);
    assert(mprotect(arena, AOS_FB_ARENA_BYTES, PROT_NONE) == 0);
    for (unsigned i = 0; i < 8; i++) {
        assert(aos_fb_pump(&client) == 0);
        assert(aos_fb_pump(peer) == 0);
    }
    assert(munmap(r, AOS_FB_CLIENT_STRIDE) == 0);
    assert(munmap(arena, AOS_FB_ARENA_BYTES) == 0);
}

static aos_fb_response_t call(aos_fb_client_t *c, aos_fb_request_t q)
{
    aos_fb_response_t p;
    q.version = AOS_FB_VERSION;
    assert(aos_fb_submit(c->region, &q) == 0);
    assert(aos_fb_pump(c) == 1);
    assert(aos_fb_receive(c->region, &p) == 0);
    assert(p.version == AOS_FB_VERSION && p.id == q.id);
    return p;
}

static uint64_t create(aos_fb_client_t *c, unsigned width, unsigned height)
{
    aos_fb_response_t p = call(c, (aos_fb_request_t){
        .operation = AOS_FB_CREATE, .width = width, .height = height });
    assert(p.status == AOS_FB_OK && p.handle && p.sequence == 0);
    assert(p.width == width && p.height == height);
    return p.handle;
}

static void exact_frame_and_clients(aos_fb_client_t *a, aos_fb_client_t *b)
{
    uint64_t handle = create(a, 8, 6);
    uint64_t other = create(b, 8, 6);
    aos_fb_request_t q = { .operation = AOS_FB_WRITE, .handle = handle,
        .x = 2, .y = 1, .width = 3, .height = 2, .data_length = 24, .data_offset = 7 };
    uint8_t expected[8 * 6 * 4] = {0};
    for (unsigned row = 0; row < 2; ++row)
        for (unsigned byte = 0; byte < 12; ++byte) {
            uint8_t value = (uint8_t)(row * 12 + byte + 1);
            a->region->data[7 + row * 12 + byte] = value;
            expected[((row + 1) * 8 + 2) * 4 + byte] = value;
        }
    assert(call(a, q).status == AOS_FB_OK);
    aos_fb_request_t read = { .operation = AOS_FB_READ, .handle = handle,
        .width = 8, .height = 6, .data_length = sizeof(expected) };
    memset(a->region->data, 0xff, sizeof(expected));
    assert(call(a, read).status == AOS_FB_OK);
    for (unsigned i = 0; i < sizeof(expected); ++i) assert(a->region->data[i] == 0);
    assert(call(a, (aos_fb_request_t){ .operation = AOS_FB_FLIP,
                                      .handle = handle }).sequence == 1);
    assert(call(a, read).status == AOS_FB_OK);
    assert(memcmp(expected, a->region->data, sizeof(expected)) == 0);
    memset(a->region->data + 7, 0xef, 24);
    assert(call(a, q).status == AOS_FB_OK);
    assert(call(a, read).sequence == 1);
    assert(memcmp(expected, a->region->data, sizeof(expected)) == 0);
    read.handle = other;
    memset(b->region->data, 0xff, sizeof(expected));
    assert(call(b, read).status == AOS_FB_OK);
    for (unsigned i = 0; i < sizeof(expected); ++i) assert(b->region->data[i] == 0);

    q.x = UINT32_MAX;
    assert(call(a, q).status == AOS_FB_BAD_BOUNDS);
    q.x = 2;
    q.data_offset = UINT32_MAX;
    assert(call(a, q).status == AOS_FB_BAD_BOUNDS);
    q.data_offset = AOS_FB_DATA_BYTES - 23;
    assert(call(a, q).status == AOS_FB_BAD_BOUNDS);
    q.data_offset = 7;
    q.data_length = 23;
    assert(call(a, q).status == AOS_FB_BAD_BOUNDS);
    assert(call(a, (aos_fb_request_t){ .operation = AOS_FB_DESTROY,
                                      .handle = handle }).status == AOS_FB_OK);
    uint64_t fresh = create(a, 8, 6);
    assert(fresh != handle);
    read.handle = handle;
    assert(call(a, read).status == AOS_FB_BAD_HANDLE);
    read.handle = fresh;
    assert(call(a, read).status == AOS_FB_OK);
    for (unsigned i = 0; i < sizeof(expected); ++i) assert(a->region->data[i] == 0);
}

static void queue_budget_backpressure_and_wrap(aos_fb_client_t *c)
{
    aos_fb_region_t *r = c->region;
    assert(r->req_head == r->req_tail && r->resp_head == r->resp_tail);
    r->req_head = r->req_tail = UINT32_MAX - 3;
    r->resp_head = r->resp_tail = UINT32_MAX - 3;
    aos_fb_request_t q = { .version = AOS_FB_VERSION, .operation = AOS_FB_STATUS,
                           .handle = c->surfaces[0].handle };
    for (unsigned i = 0; i < AOS_FB_QUEUE_CAPACITY; ++i) {
        q.id = i;
        assert(aos_fb_submit(r, &q) == 0);
    }
    assert(aos_fb_submit(r, &q) == -1);
    assert(aos_fb_pump(c) == AOS_FB_QUEUE_CAPACITY);
    for (unsigned i = 0; i < AOS_FB_QUEUE_CAPACITY; ++i) {
        q.id = AOS_FB_QUEUE_CAPACITY + i;
        assert(aos_fb_submit(r, &q) == 0);
    }
    uint32_t saved_head = r->req_head;
    assert(aos_fb_pump(c) == 0 && r->req_head == saved_head);
    aos_fb_response_t p;
    for (unsigned i = 0; i < 2 * AOS_FB_QUEUE_CAPACITY; ++i) {
        if (i == AOS_FB_QUEUE_CAPACITY)
            assert(aos_fb_pump(c) == AOS_FB_QUEUE_CAPACITY);
        assert(aos_fb_receive(r, &p) == 0);
        assert(p.id == i && p.status == AOS_FB_OK);
    }
    assert(aos_fb_receive(r, &p) == -1 && aos_fb_pump(c) == 0);
    saved_head = r->req_head;
    r->req_tail = saved_head + AOS_FB_QUEUE_CAPACITY + 1;
    assert(aos_fb_pump(c) == 0 && r->req_head == saved_head);
    assert(aos_fb_submit(r, &q) == -1);
    r->req_tail = saved_head;
    assert(aos_fb_submit(r, &q) == 0);
    uint32_t saved_out = r->resp_tail;
    r->resp_head = saved_out - AOS_FB_QUEUE_CAPACITY - 1;
    assert(aos_fb_pump(c) == 0 && r->req_head == saved_head);
    r->resp_head = saved_out;
    assert(aos_fb_pump(c) == 1);
    assert(aos_fb_receive(r, &p) == 0 && p.status == AOS_FB_OK);
}

static void capacity_and_protocol(aos_fb_client_t *c)
{
    for (unsigned i = 1; i < AOS_FB_MAX_SURFACES; ++i) create(c, 1, 1);
    aos_fb_request_t q = { .version = AOS_FB_VERSION, .operation = AOS_FB_CREATE,
                           .width = 1, .height = 1 };
    assert(call(c, q).status == AOS_FB_NO_SPACE);
    q.width = UINT32_MAX;
    assert(call(c, q).status == AOS_FB_BAD_BOUNDS);
    q.operation = 99;
    assert(call(c, q).status == AOS_FB_BAD_OPERATION);
    q.version = AOS_FB_VERSION + 1;
    assert(aos_fb_submit(c->region, &q) == 0);
    assert(aos_fb_pump(c) == 1);
    aos_fb_response_t p;
    assert(aos_fb_receive(c->region, &p) == 0 && p.status == AOS_FB_BAD_VERSION);
}

int main(void)
{
    aos_fb_client_t clients[2];
    aos_fb_region_t *regions[2];
    uint8_t *arenas[2];
    for (unsigned i = 0; i < 2; ++i) {
        regions[i] = calloc(1, sizeof(*regions[i]));
        arenas[i] = malloc(AOS_FB_ARENA_BYTES);
        assert(regions[i] && arenas[i]);
        assert(aos_fb_client_init(&clients[i], regions[i], arenas[i],
                                  AOS_FB_ARENA_BYTES - 1) == -1);
        assert(aos_fb_client_init(&clients[i], regions[i], arenas[i],
                                  AOS_FB_ARENA_BYTES) == 0);
    }
    exact_frame_and_clients(&clients[0], &clients[1]);
    queue_budget_backpressure_and_wrap(&clients[0]);
    capacity_and_protocol(&clients[0]);
    terminal_detach(&clients[1]);
    /* The unrelated client retains exact pixel I/O after retirement. */
    aos_fb_client_t *peer = &clients[1];
    uint64_t handle = peer->surfaces[0].handle;
    memset(peer->region->data, 0x6d, 8 * 6 * 4);
    assert(call(peer, (aos_fb_request_t){.operation=AOS_FB_WRITE, .handle=handle,
        .width=8, .height=6, .data_length=8*6*4}).status == AOS_FB_OK);
    assert(call(peer, (aos_fb_request_t){.operation=AOS_FB_FLIP, .handle=handle}).status == AOS_FB_OK);
    memset(peer->region->data, 0, 8 * 6 * 4);
    assert(call(peer, (aos_fb_request_t){.operation=AOS_FB_READ, .handle=handle,
        .width=8, .height=6, .data_length=8*6*4}).status == AOS_FB_OK);
    for (unsigned i=0; i<8*6*4; i++) assert(peer->region->data[i] == 0x6d);
    for (unsigned i = 0; i < 2; ++i) { free(regions[i]); free(arenas[i]); }
    puts("PASS: exact framebuffer pixels, committed snapshots, client separation, stale handles, queue backpressure/wrap/bounds");
    return 0;
}
