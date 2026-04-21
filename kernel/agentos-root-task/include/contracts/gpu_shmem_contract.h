/*
 * GPUShmem IPC Contract
 *
 * GPUShmem manages shared memory regions between host PDs and GPU hardware.
 * Callers map a region, use it for DMA, and then unmap it.  A FENCE ensures
 * coherence between host and device before the caller reads results.
 *
 * Channel: CH_GPU_SHMEM (see agentos.h)
 * Opcodes: MSG_GPUSHMEM_* (see agentos.h)
 *
 * Invariants:
 *   - MAP returns a slot; the caller must present the slot to UNMAP and FENCE.
 *   - FENCE blocks until outstanding DMA for the slot completes (host-side wait).
 *   - A slot not unmapped before the calling PD exits is leaked — the GPU
 *     hardware retains DMA access until the next system reset.
 *   - STATUS is read-only.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define GPUSHMEM_CH_CONTROLLER  CH_GPU_SHMEM

/* ─── Request structs ────────────────────────────────────────────────────── */

struct gpushmem_req_map {
    uint32_t size_pages;        /* number of 4KB pages to map */
    uint32_t flags;             /* GPUSHMEM_FLAG_* */
};

#define GPUSHMEM_FLAG_WRITE_COMBINE (1u << 0)  /* WC mapping (GPU write, host read) */
#define GPUSHMEM_FLAG_DEVICE_LOCAL  (1u << 1)  /* prefer device-local memory */

struct gpushmem_req_unmap {
    uint32_t slot;
};

struct gpushmem_req_fence {
    uint32_t slot;
};

struct gpushmem_req_status {
    /* no fields */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct gpushmem_reply_map {
    uint32_t ok;
    uint32_t slot;              /* opaque slot identifier */
    uint64_t phys_base;         /* DMA-mapped physical base address */
    uint32_t size_pages;        /* actual pages mapped (may be > requested) */
};

struct gpushmem_reply_unmap {
    uint32_t ok;
};

struct gpushmem_reply_fence {
    uint32_t ok;                /* 0 = fence complete (DMA done) */
};

struct gpushmem_reply_status {
    uint32_t total_slots;       /* maximum simultaneous mappings */
    uint32_t used_slots;
    uint64_t bytes_mapped;
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum gpushmem_error {
    GPUSHMEM_OK               = 0,
    GPUSHMEM_ERR_NO_SLOTS     = 1,  /* slot table full */
    GPUSHMEM_ERR_NO_MEM       = 2,  /* insufficient device memory */
    GPUSHMEM_ERR_BAD_SLOT     = 3,
    GPUSHMEM_ERR_FENCE_TIMEOUT = 4,
};
