#ifndef AOS_PLATFORM_BLK_VIRT_PUMP_H
#define AOS_PLATFORM_BLK_VIRT_PUMP_H

#include <platform/blk_layout.h>

/*
 * Raw ring ops on the sDDF-compatible block queues.  Safe across PDs:
 * fenced, and the head/tail indices are accessed as volatile.  The VMM side
 * uses libvmm's sddf/blk/queue.h on the same memory; blk_virt and the host
 * tests use these.
 */
uint32_t aos_blk_queue_req_length(const aos_blk_req_queue_t *q);
uint32_t aos_blk_queue_resp_length(const aos_blk_resp_queue_t *q);

void aos_blk_virt_reset(aos_blk_virt_t *v);

/*
 * Client-side helpers, header-only so a queue client (the VMM's emulated
 * virtio-blk) links no pump code: the pump lives in the blk_virt PD.
 */

/* Bind client_index's stride inside region. Does not touch buffers. */
static inline void aos_blk_client_bind(uint8_t *region, uint32_t client_index,
                                       aos_blk_virt_client_t *out)
{
    uint8_t *base;

    if (!region || !out || client_index >= AOS_BLK_MAX_CLIENTS) {
        return;
    }
    base = region + AOS_BLK_CLIENT_BASE + (client_index * AOS_BLK_CLIENT_STRIDE);
    out->info     = (aos_blk_storage_info_t *)(base + AOS_BLK_STORAGE_INFO_OFF);
    out->signal   = (aos_blk_signal_t *)(base + AOS_BLK_SIGNAL_OFF);
    out->req      = (aos_blk_req_queue_t *)(base + AOS_BLK_REQ_QUEUE_OFF);
    out->resp     = (aos_blk_resp_queue_t *)(base + AOS_BLK_RESP_QUEUE_OFF);
    out->data     = base + AOS_BLK_DATA_OFF;
    out->capacity = AOS_BLK_QUEUE_CAPACITY;
}

/* Zero the request/response queues and the signal word.  Runs before the
 * virtualizer binds the client, so plain volatile stores suffice; one fence
 * publishes the result. */
static inline void aos_blk_client_init_queues(aos_blk_virt_client_t *c)
{
    uint32_t i;

    if (!c || !c->req || !c->resp) {
        return;
    }
    for (i = 0; i < AOS_BLK_QUEUE_BYTES; i++) {
        ((volatile uint8_t *)c->req)[i] = 0u;
        ((volatile uint8_t *)c->resp)[i] = 0u;
    }
    if (c->signal) {
        for (i = 0; i < (uint32_t)sizeof(*c->signal); i++) {
            ((volatile uint8_t *)c->signal)[i] = 0u;
        }
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* Fill storage_info for a RAM disk of disk_blocks transfer units. */
void aos_blk_storage_init(aos_blk_storage_info_t *info, uint32_t disk_blocks);

int aos_blk_virt_add_client(aos_blk_virt_t *v, const aos_blk_virt_client_t *c);

void aos_blk_virt_set_disk(aos_blk_virt_t *v, uint8_t *disk, uint32_t disk_blocks);

/* Replace the RAM disk with a synchronous agentOS block backend. */
void aos_blk_virt_set_backend(aos_blk_virt_t *v, aos_blk_backend_fn backend,
                              void *ctx);

/*
 * Serve every pending request against the configured backend or RAM disk.
 * Returns I/O operations that produced a response (including invalid-param).
 */
uint32_t aos_blk_virt_pump(aos_blk_virt_t *v);

#endif /* AOS_PLATFORM_BLK_VIRT_PUMP_H */
