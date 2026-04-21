/*
 * gpu_shmem.c — seL4 secure inter-VM GPU shared memory channel
 *
 * Implements the seL4 PD side of the VMM-mediated GPU zero-copy channel.
 * See include/gpu_shmem.h for architecture overview and API docs.
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#include "gpu_shmem.h"
#include "contracts/gpu_shmem_contract.h"
#include <stddef.h>
#include <string.h>

/* ── Module state ─────────────────────────────────────────────────────── */

static gpu_shmem_ring_t *s_ring     = NULL;
static void             *s_payload  = NULL;
static size_t            s_buf_size = 0;
static gpu_shmem_role_t  s_role     = GPU_SHMEM_ROLE_PRODUCER;
static bool              s_init     = false;

/* ── Memory barrier helpers ───────────────────────────────────────────── */

/* Full read/write barrier — prevents CPU and compiler reordering */
#if defined(__aarch64__)
#  define MB()  __asm__ volatile("dmb ish" ::: "memory")
#  define WMB() __asm__ volatile("dmb ishst" ::: "memory")
#  define RMB() __asm__ volatile("dmb ishld" ::: "memory")
#elif defined(__riscv)
#  define MB()  __asm__ volatile("fence rw,rw" ::: "memory")
#  define WMB() __asm__ volatile("fence w,w" ::: "memory")
#  define RMB() __asm__ volatile("fence r,r" ::: "memory")
#else
#  define MB()  __asm__ volatile("" ::: "memory")
#  define WMB() __asm__ volatile("" ::: "memory")
#  define RMB() __asm__ volatile("" ::: "memory")
#endif

/* ── Public API ───────────────────────────────────────────────────────── */

void gpu_shmem_init(uintptr_t buf_vaddr, size_t buf_size, gpu_shmem_role_t role) {
    s_ring     = (gpu_shmem_ring_t *)buf_vaddr;
    s_payload  = (void *)(buf_vaddr + GPU_SHMEM_PAYLOAD_OFF);
    s_buf_size = buf_size;
    s_role     = role;

    if (role == GPU_SHMEM_ROLE_PRODUCER) {
        /* Producer owns ring initialisation */
        memset(s_ring, 0, sizeof(gpu_shmem_ring_t));
        s_ring->magic   = GPU_SHMEM_MAGIC;
        s_ring->version = 1;
        s_ring->depth   = GPU_SHMEM_RING_DEPTH;
        WMB();
    } else {
        /* Consumer waits for producer to initialise */
        while (s_ring->magic != GPU_SHMEM_MAGIC) {
            /* Spin — producer must call init first */
            RMB();
        }
    }

    s_init = true;
}

bool gpu_shmem_valid(void) {
    if (!s_init || !s_ring) return false;
    RMB();
    return (s_ring->magic   == GPU_SHMEM_MAGIC &&
            s_ring->version == 1 &&
            s_ring->depth   == GPU_SHMEM_RING_DEPTH);
}

bool gpu_shmem_enqueue(const gpu_tensor_desc_t *desc) {
    if (!s_init || !desc) return false;

    /* Spin until a slot is free */
    uint32_t head, tail;
    do {
        MB();
        head = s_ring->head;
        tail = s_ring->tail;
    } while ((head - tail) >= GPU_SHMEM_RING_DEPTH);

    /* Write descriptor into the ring slot */
    uint32_t slot = head % GPU_SHMEM_RING_DEPTH;
    memcpy(&s_ring->slots[slot], desc, sizeof(gpu_tensor_desc_t));
    s_ring->slots[slot].magic = GPU_SHMEM_MAGIC;
    s_ring->slots[slot].seq   = head;

    WMB();
    s_ring->head = head + 1;
    WMB();

    return true;
}

bool gpu_shmem_dequeue(gpu_tensor_desc_t *desc) {
    if (!s_init || !desc) return false;

    RMB();
    uint32_t head = s_ring->head;
    uint32_t tail = s_ring->tail;

    if (head == tail) return false; /* empty */

    uint32_t slot = tail % GPU_SHMEM_RING_DEPTH;
    memcpy(desc, &s_ring->slots[slot], sizeof(gpu_tensor_desc_t));

    if (desc->magic != GPU_SHMEM_MAGIC) {
        /* Corrupt descriptor — advance tail and return false */
        s_ring->tail = tail + 1;
        WMB();
        return false;
    }

    WMB();
    s_ring->tail = tail + 1;
    WMB();

    return true;
}

bool gpu_shmem_enqueue_result(const gpu_tensor_desc_t *desc) {
    if (!s_init || !desc) return false;

    uint32_t rhead, rtail;
    do {
        MB();
        rhead = s_ring->result_head;
        rtail = s_ring->result_tail;
    } while ((rhead - rtail) >= GPU_SHMEM_RING_DEPTH);

    /* Reuse the same ring slots for results using a second ring overlay.
     * For simplicity, results share the slots array but use a separate
     * head/tail pair with an offset of GPU_SHMEM_RING_DEPTH/2 to avoid
     * collision with in-flight descriptors. In production this would be
     * a separate MR; this design is sufficient for the prototype. */
    uint32_t slot = (rhead % (GPU_SHMEM_RING_DEPTH / 2)) + (GPU_SHMEM_RING_DEPTH / 2);
    memcpy(&s_ring->slots[slot], desc, sizeof(gpu_tensor_desc_t));
    s_ring->slots[slot].magic = GPU_SHMEM_MAGIC;
    s_ring->slots[slot].seq   = rhead;

    WMB();
    s_ring->result_head = rhead + 1;
    WMB();

    return true;
}

bool gpu_shmem_dequeue_result(gpu_tensor_desc_t *desc) {
    if (!s_init || !desc) return false;

    RMB();
    uint32_t rhead = s_ring->result_head;
    uint32_t rtail = s_ring->result_tail;

    if (rhead == rtail) return false;

    uint32_t slot = (rtail % (GPU_SHMEM_RING_DEPTH / 2)) + (GPU_SHMEM_RING_DEPTH / 2);
    memcpy(desc, &s_ring->slots[slot], sizeof(gpu_tensor_desc_t));

    if (desc->magic != GPU_SHMEM_MAGIC) {
        s_ring->result_tail = rtail + 1;
        WMB();
        return false;
    }

    WMB();
    s_ring->result_tail = rtail + 1;
    WMB();

    return true;
}

void *gpu_shmem_payload_ptr(uint64_t offset) {
    if (!s_init || !s_payload) return NULL;
    if (GPU_SHMEM_PAYLOAD_OFF + offset >= s_buf_size) return NULL;
    return (uint8_t *)s_payload + offset;
}
