#ifndef AOS_PLATFORM_SERIAL_VIRT_LAYOUT_H
#define AOS_PLATFORM_SERIAL_VIRT_LAYOUT_H

#include <platform/serial_layout.h>
#include <platform/serial_virt_pump.h>
#include <platform/blk_layout.h>

/* Each VMM and the operator map only their own large page. The frontend
 * (CC-PD on ARM, serial_pd on x86 firmware) maps only the frontend page.
 * serial_virt maps all four and is the only inter-client mux.
 * Queues are zero-initialized by root retype, never reset by ATTACH. */
#define AOS_SERIAL_CLIENTS 3u
#define AOS_SERIAL_FRAME_SIZE 0x200000u
#define AOS_SERIAL_FRAMES 4u
#define AOS_SERIAL_SHMEM_VA 0x2a000000UL
#define AOS_SERIAL_SHMEM_SIZE (AOS_SERIAL_FRAMES * AOS_SERIAL_FRAME_SIZE)
#define AOS_SERIAL_FRONTEND_FRAME 3u
#define AOS_SERIAL_FRONTEND_STRIDE 0x20000u

#define AOS_SERIAL_META_OFF 0u
#define AOS_SERIAL_TO_GUEST_QUEUE_OFF 0x1000u
#define AOS_SERIAL_FROM_GUEST_QUEUE_OFF 0x2000u
#define AOS_SERIAL_TO_GUEST_DATA_OFF 0x3000u
#define AOS_SERIAL_FROM_GUEST_DATA_OFF 0x4000u

/* VMM writes state in its own page; serial_virt mirrors it to CC's page.
 * Queue processing remains independent of guest run state, so output can
 * drain while paused and pending input can stay queued without being lost. */
typedef struct {
    uint32_t guest_state;
    uint32_t attached;
    /* Frontend page only. CC owns BUSY during queue access; serial_virt
     * permanently sets CLOSED on detach. Retyping starts with an open gate. */
    uint32_t frontend_gate;
} aos_serial_channel_meta_t;
#define AOS_SERIAL_FRONTEND_CLOSED 1u
#define AOS_SERIAL_FRONTEND_BUSY 2u

typedef struct {
    aos_serial_queue_handle_t to_guest;
    aos_serial_queue_handle_t from_guest;
    aos_serial_channel_meta_t *meta;
} aos_serial_channel_t;

static inline aos_serial_channel_t aos_serial_channel_at(uintptr_t base)
{
    return (aos_serial_channel_t) {
        .to_guest = {
            (aos_serial_queue_t *)(base + AOS_SERIAL_TO_GUEST_QUEUE_OFF),
            (uint8_t *)(base + AOS_SERIAL_TO_GUEST_DATA_OFF), AOS_SERIAL_RX_CAPACITY},
        .from_guest = {
            (aos_serial_queue_t *)(base + AOS_SERIAL_FROM_GUEST_QUEUE_OFF),
            (uint8_t *)(base + AOS_SERIAL_FROM_GUEST_DATA_OFF), AOS_SERIAL_TX_CAPACITY},
        .meta = (aos_serial_channel_meta_t *)(base + AOS_SERIAL_META_OFF),
    };
}

_Static_assert(sizeof(aos_serial_queue_t) == 12u, "sDDF serial queue ABI");
_Static_assert(AOS_SERIAL_TO_GUEST_DATA_OFF + AOS_SERIAL_RX_CAPACITY <=
               AOS_SERIAL_FROM_GUEST_DATA_OFF, "serial data regions are disjoint");
_Static_assert(AOS_SERIAL_FROM_GUEST_DATA_OFF + AOS_SERIAL_TX_CAPACITY <=
               AOS_SERIAL_FRONTEND_STRIDE, "serial channel fits frontend stride");
_Static_assert(AOS_SERIAL_CLIENTS * AOS_SERIAL_FRONTEND_STRIDE <=
               AOS_SERIAL_FRAME_SIZE, "frontend channels fit one isolated page");
_Static_assert(AOS_SERIAL_SHMEM_VA >= AOS_BLK_SHMEM_VA + AOS_BLK_SHMEM_SIZE,
               "serial and block mappings are disjoint");
#endif
