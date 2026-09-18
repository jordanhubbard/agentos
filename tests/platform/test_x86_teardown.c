#include <platform/guest_teardown.h>
#include <platform/guest_ram.h>
#include <libvmm/virtio/gpa.h>
#include "contracts/guest_execution_caps.h"
#include "contracts/guest_queue_caps.h"
#include "contracts/x86_guest_memory_caps.h"
#include <sel4/sel4.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static const seL4_CPtr slots[] = {
    AOS_GUEST_QUEUE_POOL_BASE, AOS_GUEST_QUEUE_POOL_BASE + 1u,
    AOS_GUEST_QUEUE_POOL_BASE + 2u, AOS_GUEST_EXECUTION_POOL_CAP,
    AOS_GUEST_RAM_POOL_BASE, AOS_GUEST_RAM_POOL_BASE + 1u,
    AOS_X86_GUEST_ROM_POOL_BASE, AOS_X86_GUEST_ROM_POOL_BASE + 1u,
};
static bool console_done, block_done, net_detached, block_detached, serial_detached;
static bool disabled, fail_armed, released[8];
static unsigned fail_index, calls[8], device_calls;
static size_t page_size;
static volatile unsigned char *ram;

static void touch(void) { assert(*ram == 0x5a); device_calls++; }
void aos_vmm_virtio_net_quiesce(void) { touch(); }
bool aos_vmm_virtio_console_quiesce(void) { touch(); return console_done; }
bool aos_vmm_virtio_blk_quiesce(void) { touch(); return block_done; }
bool aos_vmm_virtio_net_detach(void) { touch(); return net_detached; }
bool aos_vmm_virtio_blk_detach(void) { touch(); return block_detached; }
bool aos_vmm_serial_detach(void) { touch(); return serial_detached; }
void virtio_gpa_set_translate(virtio_gpa_translate_fn fn)
{
    assert(fn == NULL);
    disabled = true;
}

seL4_Error seL4_CNode_Revoke(seL4_CPtr root, seL4_Word slot, uint8_t depth)
{
    assert(root == AOS_GUEST_RAM_SELF_CNODE && depth == AOS_GUEST_RAM_CNODE_BITS);
    assert(console_done && block_done && net_detached && block_detached && serial_detached);
    unsigned index = 0;
    while (index < 8u && slots[index] != slot) index++;
    assert(index < 8u); /* In particular, never revoke ARM's paging pool. */
    for (unsigned prior = 0; prior < index; prior++) assert(released[prior]);
    if (index >= 4u) {
        assert(disabled && released[3]);
        /* A revoke can partially succeed before reporting failure. Any late
         * device or queue access, including on retry, must fault this test. */
        assert(mprotect((void *)ram, page_size, PROT_NONE) == 0);
    }
    calls[index]++;
    if (fail_armed && index == fail_index) {
        fail_armed = false;
        return 1;
    }
    released[index] = true;
    return seL4_NoError;
}

static void reset(void)
{
    assert(mprotect((void *)ram, page_size, PROT_READ | PROT_WRITE) == 0);
    *ram = 0x5a;
    memset(calls, 0, sizeof(calls));
    memset(released, 0, sizeof(released));
    console_done = block_done = net_detached = block_detached = serial_detached = false;
    disabled = fail_armed = false;
    device_calls = 0;
}

int main(void)
{
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    ram = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(ram != MAP_FAILED);
    const size_t size = (size_t)2u << AOS_GUEST_RAM_FRAME_BITS;
    for (unsigned failure = 0; failure < 8u; failure++) {
        reset();
        aos_guest_teardown_t state = {0};
        assert(!aos_guest_teardown_step(NULL, size));
        assert(!aos_guest_teardown_step(&state, 0));
        assert(!aos_guest_teardown_step(&state, size));
        assert(device_calls == 3u && !calls[0]);
        console_done = true;
        assert(!aos_guest_teardown_step(&state, size) && !calls[0]);
        block_done = true;
        assert(!aos_guest_teardown_step(&state, size) && !calls[0]);
        net_detached = true;
        assert(!aos_guest_teardown_step(&state, size) && !calls[0]);
        block_detached = true;
        assert(!aos_guest_teardown_step(&state, size) && !calls[0]);
        serial_detached = true;
        fail_index = failure;
        fail_armed = true;
        assert(!aos_guest_teardown_step(&state, size));
        assert(calls[failure] == 1u && !released[failure]);
        unsigned before = device_calls;
        assert(aos_guest_teardown_step(&state, size));
        assert(device_calls == before && disabled);
        assert(state.execution_released && state.ram_released && state.paging_released);
        for (unsigned i = 0; i < 8u; i++) assert(released[i]);
        assert(calls[failure] == 2u);
        assert(aos_guest_teardown_step(&state, size));
        assert(device_calls == before && calls[failure] == 2u);
    }
    reset();
    assert(!aos_vmm_guest_ram_release(0));
    assert(!aos_vmm_guest_ram_release(size + 1u));
    assert(!aos_vmm_guest_ram_release(((size_t)AOS_GUEST_RAM_MAX_FRAMES + 1u) << AOS_GUEST_RAM_FRAME_BITS));
    assert(!aos_vmm_guest_ram_release(SIZE_MAX));
    assert(!disabled && !calls[0]);
    assert(munmap((void *)ram, page_size) == 0);
    puts("PASS: x86 drain/detach ordering, every revoke failure, partial-release retry and retired-memory protection");
}
