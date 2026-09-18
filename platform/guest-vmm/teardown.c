#include <platform/guest_teardown.h>
#include <platform/guest_ram.h>
#include <platform/vmm_virtio_net.h>
#include <platform/vmm_virtio_blk.h>
#include <platform/vmm_virtio_console.h>
#ifdef AGENTOS_GUEST_GRAPHICS
#include <platform/vmm_virtio_gpu.h>
#endif
#ifdef AGENTOS_GUEST_INPUT
#include <platform/vmm_virtio_input.h>
#endif
#include "contracts/guest_execution_caps.h"
#include "contracts/guest_ram_caps.h"
#include "contracts/guest_paging_caps.h"
#include <sel4/sel4.h>

bool aos_guest_teardown_step(aos_guest_teardown_t *state, size_t ram_size)
{
    if (state == NULL || ram_size == 0u) return false;
    if (!state->devices_quiesced) {
        aos_vmm_virtio_net_quiesce();
#ifdef AGENTOS_GUEST_INPUT
        aos_vmm_virtio_input_quiesce();
#endif
        /* Do not short-circuit: stop admission on every device even when
         * one backend still needs service notifications to finish. */
        bool console_done = aos_vmm_virtio_console_quiesce();
        bool block_done = aos_vmm_virtio_blk_quiesce();
        bool graphics_done = true;
#ifdef AGENTOS_GUEST_GRAPHICS
        graphics_done = aos_vmm_virtio_gpu_quiesce();
#endif
        if (!console_done || !block_done || !graphics_done) return false;
        state->devices_quiesced = true;
    }
    if (!state->network_detached) {
        if (!aos_vmm_virtio_net_detach()) return false;
        state->network_detached = true;
    }
    if (!state->block_detached) {
        if (!aos_vmm_virtio_blk_detach()) return false;
        state->block_detached = true;
    }
    if (!state->serial_detached) {
        if (!aos_vmm_serial_detach()) return false;
        state->serial_detached = true;
    }
    if (!state->input_detached) {
#ifdef AGENTOS_GUEST_INPUT
        if (!aos_vmm_virtio_input_detach()) return false;
#endif
        state->input_detached = true;
    }
    if (!state->execution_released) {
        if (seL4_CNode_Revoke(AOS_GUEST_RAM_SELF_CNODE,
                AOS_GUEST_EXECUTION_POOL_CAP, AOS_GUEST_RAM_CNODE_BITS)
                != seL4_NoError) return false;
        state->execution_released = true;
    }
    if (!state->ram_released) {
        if (!aos_vmm_guest_ram_release(ram_size)) return false;
        state->ram_released = true;
    }
    if (!state->paging_released) {
        if (seL4_CNode_Revoke(AOS_GUEST_RAM_SELF_CNODE,
                AOS_GUEST_PAGING_POOL_CAP, AOS_GUEST_RAM_CNODE_BITS)
                != seL4_NoError) return false;
        state->paging_released = true;
    }
    return true;
}
