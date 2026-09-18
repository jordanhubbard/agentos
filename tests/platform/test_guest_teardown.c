#include <platform/guest_teardown.h>
#include "contracts/guest_execution_caps.h"
#include "contracts/guest_ram_caps.h"
#include <sel4/sel4.h>
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static bool console_done, block_done, graphics_done, revoke_fails, ram_fails;
static unsigned net_calls, input_calls, console_calls, block_calls, gpu_calls;
static unsigned revokes, releases;
static volatile unsigned char *ram;
static size_t page_size;
static bool caps_live = true;
static void device_access(void) { assert(caps_live); assert(*ram == 0x5a); }
void aos_vmm_virtio_net_quiesce(void) { device_access(); net_calls++; }
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
    assert(slot == AOS_GUEST_EXECUTION_POOL_CAP);
    assert(depth == AOS_GUEST_RAM_CNODE_BITS);
    assert(console_done && block_done && graphics_done);
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
    assert(aos_guest_teardown_step(&state, page_size));
    assert(aos_guest_teardown_step(&state, page_size));
    assert(state.ram_released && revokes == 2 && releases == 2);
    assert(net_calls == device_calls && input_calls == device_calls &&
           console_calls == device_calls && block_calls == device_calls);
    assert(munmap((void *)ram, page_size) == 0);
    puts("PASS: teardown retains RAM during drain and retries partial revocation without stale callbacks");
}
