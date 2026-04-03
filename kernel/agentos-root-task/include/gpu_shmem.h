/*
 * gpu_shmem.h — seL4 secure inter-VM GPU shared memory channel
 *
 * Provides a zero-copy tensor exchange channel between agentOS native PDs
 * (WASM slots, workers) and CUDA/PyTorch workloads running in the Linux
 * guest VM on sparky (GB10, 128GB unified VRAM).
 *
 * Architecture:
 *   seL4 MR (gpu_tensor_buf) ← mapped into both linux_vmm PD and controller
 *   A ring-buffer descriptor (gpu_shmem_ring_t) sits at the start of the MR.
 *   Producers write tensor descriptors; consumers read them.
 *   Notification channels signal ready/consumed events.
 *
 * Physical layout of gpu_tensor_buf MR (64MB):
 *   [0x000000 .. 0x000FFF] control ring (4KB): gpu_shmem_ring_t
 *   [0x001000 .. 0x3FFFFF] tensor payload area (64MB - 4KB)
 *
 * Tensor descriptors are 64 bytes each; the ring holds up to
 * GPU_SHMEM_RING_DEPTH (64) outstanding descriptors.
 *
 * Usage (seL4 PD side):
 *   // In init():
 *   gpu_shmem_init(gpu_tensor_buf_vaddr, GPU_SHMEM_BUF_SIZE, GPU_SHMEM_ROLE_PRODUCER);
 *   // To enqueue a tensor:
 *   gpu_tensor_desc_t desc = { .offset = ..., .size = ..., .dtype = ..., .shape = ... };
 *   gpu_shmem_enqueue(&desc);
 *   microkit_notify(VMM_GPU_NOTIFY_CH);  // wake linux guest
 *
 * Usage (Linux guest side — see userspace/gpu_shmem_linux/):
 *   The Linux kernel module maps the same PA via /dev/gpu_shmem,
 *   reads descriptors, and dispatches PyTorch operations.
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#pragma once
#ifndef GPU_SHMEM_H
#define GPU_SHMEM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Constants ────────────────────────────────────────────────────────── */

#define GPU_SHMEM_MAGIC       0xA6E05F01UL  /* "agentOS GPU shmem v1" */
#define GPU_SHMEM_BUF_SIZE    0x4000000UL   /* 64MB total MR */
#define GPU_SHMEM_RING_DEPTH  64            /* max outstanding tensor ops */
#define GPU_SHMEM_PAYLOAD_OFF 0x1000        /* payload area starts at 4KB */

/* Tensor data types (subset of torch.dtype) */
typedef enum {
    GPU_DTYPE_F32  = 0,
    GPU_DTYPE_F16  = 1,
    GPU_DTYPE_BF16 = 2,
    GPU_DTYPE_I32  = 3,
    GPU_DTYPE_I64  = 4,
    GPU_DTYPE_U8   = 5,
} gpu_dtype_t;

/* Operation codes for tensor descriptors */
typedef enum {
    GPU_OP_INFER   = 0,   /* Run forward pass on tensor, return result */
    GPU_OP_COPY_IN = 1,   /* Copy tensor from seL4 MR into GPU VRAM */
    GPU_OP_COPY_OUT= 2,   /* Copy tensor result from GPU VRAM to MR */
    GPU_OP_BARRIER = 3,   /* Synchronization barrier — both sides halt until acked */
} gpu_op_t;

/* ── Tensor descriptor (64 bytes, cache-line aligned) ─────────────────── */

typedef struct __attribute__((packed, aligned(64))) {
    uint32_t   magic;          /* GPU_SHMEM_MAGIC for validity check */
    uint8_t    op;             /* gpu_op_t — operation to perform */
    uint8_t    dtype;          /* gpu_dtype_t — element data type */
    uint8_t    ndim;           /* Number of dimensions (≤ 4) */
    uint8_t    _pad0;
    uint32_t   shape[4];       /* Dimension sizes (unused dims = 0) */
    uint64_t   offset;         /* Byte offset into payload area */
    uint64_t   size;           /* Byte size of tensor data */
    uint64_t   result_offset;  /* Byte offset for result (GPU_OP_INFER) */
    uint64_t   result_size;    /* Byte size of result tensor */
    uint32_t   seq;            /* Sequence number (monotonic) */
    uint32_t   flags;          /* Reserved */
} gpu_tensor_desc_t;

/* Verify descriptor is exactly 64 bytes */
_Static_assert(sizeof(gpu_tensor_desc_t) == 64,
               "gpu_tensor_desc_t must be 64 bytes");

/* ── Ring buffer header (sits at offset 0 of the MR) ─────────────────── */

typedef struct __attribute__((aligned(64))) {
    uint32_t         magic;       /* GPU_SHMEM_MAGIC */
    uint32_t         version;     /* Protocol version (1) */
    uint32_t         depth;       /* Ring depth (GPU_SHMEM_RING_DEPTH) */
    uint32_t         _pad;
    /* Producer writes to head; consumer reads from tail.
     * Both are monotonically increasing sequence numbers.
     * Full: head - tail == depth.  Empty: head == tail. */
    volatile uint32_t head;       /* Next slot to write (producer) */
    volatile uint32_t tail;       /* Next slot to read  (consumer) */
    volatile uint32_t result_head;/* Producer of results (Linux side) */
    volatile uint32_t result_tail;/* Consumer of results (seL4 side) */
    uint8_t          _pad2[32];
    gpu_tensor_desc_t slots[GPU_SHMEM_RING_DEPTH];
} gpu_shmem_ring_t;

_Static_assert(offsetof(gpu_shmem_ring_t, slots) == 64,
               "gpu_shmem_ring_t.slots must start at byte 64");

/* ── Role ─────────────────────────────────────────────────────────────── */

typedef enum {
    GPU_SHMEM_ROLE_PRODUCER = 0,  /* seL4 PD sending tensors to Linux */
    GPU_SHMEM_ROLE_CONSUMER = 1,  /* linux_vmm PD receiving from Linux */
} gpu_shmem_role_t;

/* ── API ──────────────────────────────────────────────────────────────── */

/*
 * Initialize the shared memory region.
 * buf_vaddr: virtual address of the MR in this PD's address space
 * buf_size:  total size of the MR (must be >= GPU_SHMEM_BUF_SIZE)
 * role:      PRODUCER (seL4 → Linux) or CONSUMER (Linux → seL4)
 */
void gpu_shmem_init(uintptr_t buf_vaddr, size_t buf_size, gpu_shmem_role_t role);

/*
 * Enqueue a tensor descriptor (producer side).
 * Blocks (spin) until a slot is available.
 * Returns true on success, false if ring is in error state.
 */
bool gpu_shmem_enqueue(const gpu_tensor_desc_t *desc);

/*
 * Dequeue a pending tensor descriptor (consumer side / result poller).
 * Returns true and fills *desc if a descriptor is ready; false if empty.
 * Non-blocking — call from notified() handler.
 */
bool gpu_shmem_dequeue(gpu_tensor_desc_t *desc);

/*
 * Enqueue a result descriptor back to the seL4 side (Linux side).
 */
bool gpu_shmem_enqueue_result(const gpu_tensor_desc_t *desc);

/*
 * Dequeue a result descriptor (seL4 side polling after notify).
 */
bool gpu_shmem_dequeue_result(gpu_tensor_desc_t *desc);

/*
 * Return a pointer to the payload area at the given offset.
 * The caller must not access beyond offset + size.
 */
void *gpu_shmem_payload_ptr(uint64_t offset);

/*
 * Validate the ring header (magic, version, depth).
 */
bool gpu_shmem_valid(void);

#endif /* GPU_SHMEM_H */
