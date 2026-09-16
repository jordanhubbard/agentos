/* Exercise the retired PD's real IPC handlers, not a copied model. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../services/legacy-pds/framebuffer_pd.c"

static sel4_msg_t create(uint32_t backend, uint32_t expected_status)
{
    uint32_t fields[] = {16, 16, FB_FMT_XRGB8888, backend};
    memcpy((void *)fb_shmem_vaddr, fields, sizeof(fields));
    sel4_msg_t request = {.opcode = MSG_FB_CREATE};
    sel4_msg_t reply = {0};
    assert(framebuffer_pd_dispatch_one(1, &request, &reply) == expected_status);
    assert(reply.length == 8);
    return reply;
}

int main(void)
{
    void *memory = calloc(1, FB_SHMEM_SIZE);
    assert(memory);
    fb_shmem_vaddr = (uintptr_t)memory;
    framebuffer_pd_test_init();

    /* Rejection must never allocate a surface or silently select NULL. */
    for (unsigned i = 0; i < FB_MAX_SURFACES + 1; ++i) {
        sel4_msg_t reply = create(FB_BACKEND_HW_DIRECT, SEL4_ERR_BAD_ARG);
        assert(data_rd32(reply.data, 0) == FB_ERR_BAD_BACKEND);
        assert(data_rd32(reply.data, 4) == FB_HANDLE_INVALID);
    }

    uint32_t handles[FB_MAX_SURFACES];
    for (unsigned i = 0; i < FB_MAX_SURFACES; ++i) {
        sel4_msg_t reply = create(FB_BACKEND_NULL, SEL4_ERR_OK);
        assert(data_rd32(reply.data, 0) == FB_OK);
        handles[i] = data_rd32(reply.data, 4);
        assert(handles[i] != FB_HANDLE_INVALID);
        for (unsigned j = 0; j < i; ++j) assert(handles[i] != handles[j]);
    }

    sel4_msg_t full = create(FB_BACKEND_NULL, SEL4_ERR_NO_MEM);
    assert(data_rd32(full.data, 0) == FB_ERR_NO_SLOTS);
    for (unsigned i = 0; i < FB_MAX_SURFACES; ++i) {
        sel4_msg_t request = {.opcode = MSG_FB_FLIP};
        sel4_msg_t reply = {0};
        data_wr32(request.data, 0, handles[i]);
        assert(framebuffer_pd_dispatch_one(1, &request, &reply) == SEL4_ERR_OK);
        assert(data_rd32(reply.data, 0) == FB_OK);
        assert(data_rd32(reply.data, 4) == 1);
        request.opcode = MSG_FB_DESTROY;
        assert(framebuffer_pd_dispatch_one(1, &request, &reply) == SEL4_ERR_OK);
        assert(data_rd32(reply.data, 0) == FB_OK);
    }
    free(memory);
    puts("PASS: retired framebuffer rejects hardware creation without consuming slots");
    return 0;
}
