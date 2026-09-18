#include <platform/guest_teardown.h>
#include "contracts/guest_execution_caps.h"
#include "contracts/guest_ram_caps.h"
#include "contracts/guest_paging_caps.h"
#include "contracts/guest_queue_caps.h"
#include "contracts/guest_graphics_caps.h"
#include <sel4/sel4.h>
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static bool console_done, block_done, graphics_done, revoke_fails, ram_fails;
static bool paging_fails;
static bool detach_done;
static unsigned detach_calls;
static bool block_detach_done;
static unsigned block_detach_calls;
static bool serial_detach_done;
static unsigned serial_detach_calls;
static bool input_detach_done;
static unsigned input_detach_calls;
static bool graphics_detach_done;
static unsigned graphics_detach_calls;
static unsigned paging_revokes;
static unsigned queue_revokes[AOS_GUEST_QUEUE_POOL_COUNT];
static bool queue_fails;
static unsigned graphics_revokes[AOS_GUEST_GRAPHICS_POOL_COUNT];
static bool graphics_revoke_fails;
static unsigned net_calls, input_calls, console_calls, block_calls, gpu_calls;
static unsigned revokes, releases;
static volatile unsigned char *ram;
static size_t page_size;
static bool caps_live = true;
static void device_access(void) { assert(caps_live); assert(*ram == 0x5a); }
void aos_vmm_virtio_net_quiesce(void) { device_access(); net_calls++; }
bool aos_vmm_virtio_net_detach(void)
{ device_access(); detach_calls++; return detach_done; }
bool aos_vmm_virtio_blk_detach(void)
{ device_access(); block_detach_calls++; return block_detach_done; }
bool aos_vmm_serial_detach(void)
{ device_access(); serial_detach_calls++; return serial_detach_done; }
bool aos_vmm_virtio_input_detach(void)
{ device_access(); input_detach_calls++; return input_detach_done; }
bool aos_vmm_virtio_gpu_detach(void)
{ device_access(); graphics_detach_calls++; return graphics_detach_done; }
void aos_vmm_virtio_input_quiesce(void) { device_access(); input_calls++; }
bool aos_vmm_virtio_console_quiesce(void)
{ device_access(); console_calls++; return console_done; }
bool aos_vmm_virtio_blk_quiesce(void)
{ device_access(); block_calls++; return block_done; }
bool aos_vmm_virtio_gpu_quiesce(void)
{ device_access(); gpu_calls++; return graphics_done; }
seL4_Error seL4_CNode_Revoke(seL4_CPtr root, seL4_Word slot, uint8_t depth)
{
    assert(root == AOS_GUEST_RAM_SELF_CNODE);
    assert(depth == AOS_GUEST_RAM_CNODE_BITS);
    assert(console_done && block_done && graphics_done);
    assert(detach_done);
    assert(block_detach_done);
    assert(serial_detach_done);
    assert(input_detach_done);
    assert(graphics_detach_done);
    if (slot >= AOS_GUEST_QUEUE_POOL_BASE &&
        slot < AOS_GUEST_QUEUE_POOL_BASE + AOS_GUEST_QUEUE_POOL_COUNT) {
        unsigned index = slot - AOS_GUEST_QUEUE_POOL_BASE;
        assert(caps_live && !revokes && !releases);
        for (unsigned i = 0; i < index; i++) assert(queue_revokes[i] > 0);
        queue_revokes[index]++;
        return queue_fails && index == AOS_GUEST_QUEUE_BLOCK ? 1 : seL4_NoError;
    }
    for (unsigned i = 0; i < AOS_GUEST_QUEUE_POOL_COUNT; i++)
        assert(queue_revokes[i] == (i == AOS_GUEST_QUEUE_BLOCK ? 2u : 1u));
    if (slot >= AOS_GUEST_GRAPHICS_POOL_BASE &&
        slot < AOS_GUEST_GRAPHICS_POOL_BASE + AOS_GUEST_GRAPHICS_POOL_COUNT) {
        unsigned index = slot - AOS_GUEST_GRAPHICS_POOL_BASE;
        assert(caps_live && !revokes && !releases);
        for (unsigned i = 0; i < index; i++) assert(graphics_revokes[i] > 0);
        graphics_revokes[index]++;
        return graphics_revoke_fails && index == 7u ? 1 : seL4_NoError;
    }
    for (unsigned i = 0; i < AOS_GUEST_GRAPHICS_POOL_COUNT; i++)
        assert(graphics_revokes[i] == (i == 7u ? 2u : 1u));
    if (slot == AOS_GUEST_PAGING_POOL_CAP) {
        assert(!caps_live && releases == 2 && !ram_fails);
        paging_revokes++;
        return paging_fails ? 1 : seL4_NoError;
    }
    assert(slot == AOS_GUEST_EXECUTION_POOL_CAP);
    revokes++;
    if (revoke_fails) return 1;
    caps_live = false;
    return seL4_NoError;
}
bool aos_vmm_guest_ram_release(size_t size)
{
    assert(size == page_size && !caps_live);
    releases++;
    /* Even a failed release may already have unmapped part of guest RAM. */
    assert(mprotect((void *)ram, page_size, PROT_NONE) == 0);
    return !ram_fails;
}
int main(void)
{
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    ram = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(ram != MAP_FAILED);
    *ram = 0x5a;
    aos_guest_teardown_t state = {0};
    assert(!aos_guest_teardown_step(NULL, page_size));
    assert(!aos_guest_teardown_step(&state, 0));
    assert(!aos_guest_teardown_step(&state, page_size));
    assert(net_calls == 1 && input_calls == 1 && console_calls == 1 &&
           block_calls == 1 && gpu_calls == 1 && !revokes && !releases);
    console_done = true;
    assert(!aos_guest_teardown_step(&state, page_size) && !revokes);
    block_done = true;
    assert(!aos_guest_teardown_step(&state, page_size) && !revokes);
    graphics_done = true;
    assert(!aos_guest_teardown_step(&state, page_size));
    assert(state.devices_quiesced && !state.network_detached && !revokes);
    assert(detach_calls == 1);
    detach_done = true;
    assert(!aos_guest_teardown_step(&state, page_size));
    assert(state.network_detached && !state.block_detached && !revokes);
    assert(detach_calls == 2 && block_detach_calls == 1);
    block_detach_done = true;
    assert(!aos_guest_teardown_step(&state, page_size));
    assert(state.block_detached && !state.serial_detached && !revokes);
    assert(block_detach_calls == 2 && serial_detach_calls == 1);
    serial_detach_done = true;
    assert(!aos_guest_teardown_step(&state, page_size));
    assert(state.serial_detached && !state.input_detached && !revokes);
    assert(serial_detach_calls == 2 && input_detach_calls == 1);
    input_detach_done = true;
    assert(!aos_guest_teardown_step(&state, page_size));
    assert(state.input_detached && !state.graphics_detached && !revokes);
    assert(graphics_detach_calls == 1 && !queue_revokes[0]);
    graphics_detach_done = true;
    queue_fails = true;
    assert(!aos_guest_teardown_step(&state, page_size));
    assert(state.input_detached && state.queue_pools_released == 1);
    assert(queue_revokes[0] == 1 && queue_revokes[1] == 1);
    assert(!queue_revokes[2] && !queue_revokes[3] && !revokes && !releases);
    queue_fails = false;
    graphics_revoke_fails = true;
    assert(!aos_guest_teardown_step(&state, page_size));
    assert(state.graphics_pools_released == 7 && !revokes && !releases);
    assert(graphics_revokes[7] == 1 && !graphics_revokes[8]);
    graphics_revoke_fails = false;
    revoke_fails = true;
    assert(!aos_guest_teardown_step(&state, page_size));
    assert(state.devices_quiesced && !state.execution_released && !releases);
    unsigned device_calls = gpu_calls;
    revoke_fails = false;
    ram_fails = true;
    assert(!aos_guest_teardown_step(&state, page_size));
    assert(state.execution_released && !state.ram_released);
    assert(revokes == 2 && releases == 1 && gpu_calls == device_calls);
    /* Retrying partial RAM release must invoke no device or execution API. */
    ram_fails = false;
    paging_fails = true;
    assert(!aos_guest_teardown_step(&state, page_size));
    assert(state.ram_released && !state.paging_released && paging_revokes == 1);
    paging_fails = false;
    assert(aos_guest_teardown_step(&state, page_size));
    assert(aos_guest_teardown_step(&state, page_size));
    assert(state.ram_released && revokes == 2 && releases == 2);
    assert(state.paging_released && paging_revokes == 2);
    assert(state.network_detached && detach_calls == 2);
    assert(state.block_detached && block_detach_calls == 2);
    assert(state.serial_detached && serial_detach_calls == 2);
    assert(state.input_detached && input_detach_calls == 2);
    assert(state.graphics_detached && graphics_detach_calls == 2);
    assert(state.queue_pools_released == AOS_GUEST_QUEUE_POOL_COUNT);
    assert(state.graphics_pools_released == AOS_GUEST_GRAPHICS_POOL_COUNT);
    assert(net_calls == device_calls && input_calls == device_calls &&
           console_calls == device_calls && block_calls == device_calls);
    assert(munmap((void *)ram, page_size) == 0);
    puts("PASS: teardown retains RAM during drain and retries partial revocation without stale callbacks");
}
