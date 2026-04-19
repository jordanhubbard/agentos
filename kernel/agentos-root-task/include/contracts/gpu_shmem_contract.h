#pragma once
/* GPU_SHMEM contract — version 1
 * PD: gpu_shmem | Source: src/gpu_shmem.c | Channel: (mapped via root task grant; no fixed PPC channel)
 */
#include <stdint.h>
#include <stdbool.h>

#define GPU_SHMEM_CONTRACT_VERSION 1

/* ── Opcodes (0xB500 range) ── */
#define GPU_SHMEM_OP_MAP        0xB501u  /* map a GPU shared memory region to a caller */
#define GPU_SHMEM_OP_UNMAP      0xB502u  /* release a previously mapped region */
#define GPU_SHMEM_OP_FENCE      0xB503u  /* insert a memory fence / sync point */
#define GPU_SHMEM_OP_STATUS     0xB504u  /* query mapped regions and fence state */

/* ── Region flags ── */
#define GPU_SHMEM_FLAG_READ     (1u << 0)  /* region is readable by caller */
#define GPU_SHMEM_FLAG_WRITE    (1u << 1)  /* region is writable by caller */
#define GPU_SHMEM_FLAG_COHERENT (1u << 2)  /* cache-coherent with GPU */
#define GPU_SHMEM_FLAG_UNCACHED (1u << 3)  /* bypass CPU cache (device memory) */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* GPU_SHMEM_OP_MAP */
    uint32_t size_pages;      /* region size in 4KB pages */
    uint32_t flags;           /* GPU_SHMEM_FLAG_* bitmask */
    uint32_t align_log2;      /* alignment requirement (e.g. 21 = 2MB) */
} gpu_shmem_req_map_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else gpu_shmem_error_t */
    uint32_t region_id;       /* opaque handle for unmap/fence */
    uint64_t vaddr;           /* virtual address of mapped region in caller's space */
    uint32_t actual_pages;    /* pages actually mapped (>= size_pages, aligned) */
} gpu_shmem_reply_map_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* GPU_SHMEM_OP_UNMAP */
    uint32_t region_id;
} gpu_shmem_req_unmap_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} gpu_shmem_reply_unmap_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* GPU_SHMEM_OP_FENCE */
    uint32_t region_id;
    uint32_t fence_type;      /* GPU_SHMEM_FENCE_* */
} gpu_shmem_req_fence_t;

#define GPU_SHMEM_FENCE_READ    0u  /* ensure prior writes visible to GPU reads */
#define GPU_SHMEM_FENCE_WRITE   1u  /* ensure GPU writes visible to CPU reads */
#define GPU_SHMEM_FENCE_FULL    2u  /* full bidirectional fence */

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint64_t fence_seq;       /* monotonic fence sequence number */
} gpu_shmem_reply_fence_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* GPU_SHMEM_OP_STATUS */
} gpu_shmem_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t mapped_regions;  /* number of currently mapped regions */
    uint32_t total_pages;     /* total pages in use */
    uint32_t free_pages;      /* pages available for mapping */
    uint64_t fence_seq;       /* last completed fence sequence number */
} gpu_shmem_reply_status_t;

/* ── Error codes ── */
typedef enum {
    GPU_SHMEM_OK              = 0,
    GPU_SHMEM_ERR_NO_MEM      = 1,  /* insufficient GPU memory pages */
    GPU_SHMEM_ERR_BAD_REGION  = 2,  /* region_id invalid or already unmapped */
    GPU_SHMEM_ERR_BAD_FLAGS   = 3,  /* conflicting or unsupported flags */
    GPU_SHMEM_ERR_NO_CAP      = 4,  /* caller lacks AGENTOS_CAP_GPU */
    GPU_SHMEM_ERR_ALIGN       = 5,  /* requested alignment unsupported */
    GPU_SHMEM_ERR_FENCE_BUSY  = 6,  /* previous fence not yet retired */
} gpu_shmem_error_t;

/* ── Invariants ──
 * - Callers must hold AGENTOS_CAP_GPU.
 * - GPU_SHMEM_FLAG_UNCACHED and GPU_SHMEM_FLAG_COHERENT are mutually exclusive.
 * - UNMAP is idempotent only for regions in the caller's own set.
 * - Fence operations are ordered: fence_seq is monotonically increasing.
 * - All regions are automatically unmapped when the caller PD is destroyed.
 */
