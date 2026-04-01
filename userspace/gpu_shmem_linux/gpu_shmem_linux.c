/*
 * gpu_shmem_linux.c — Linux userspace shim for seL4 GPU shared memory channel
 *
 * Runs in the Linux guest VM on sparky (GB10).  Maps the shared seL4 MR
 * via /dev/mem or a dedicated UIO device, polls for incoming tensor
 * descriptors, and dispatches them to PyTorch/CUDA via libcuda.
 *
 * This is a USERSPACE daemon (not a kernel module) for prototype purposes.
 * In production, a UIO or virtIO driver would handle this in-kernel.
 *
 * Compile: gcc -std=c11 -O2 gpu_shmem_linux.c -o gpu_shmem_linux -lcuda
 * Run:     sudo ./gpu_shmem_linux --pa 0xXXXXXXXX --size 0x4000000
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <signal.h>

/* Re-include gpu_shmem ring types (duplicated here for portability) */
#define GPU_SHMEM_MAGIC       0xA6E05F01UL
#define GPU_SHMEM_BUF_SIZE    0x4000000UL
#define GPU_SHMEM_RING_DEPTH  64
#define GPU_SHMEM_PAYLOAD_OFF 0x1000

typedef enum { GPU_DTYPE_F32=0, GPU_DTYPE_F16=1, GPU_DTYPE_BF16=2,
               GPU_DTYPE_I32=3, GPU_DTYPE_I64=4, GPU_DTYPE_U8=5 } gpu_dtype_t;
typedef enum { GPU_OP_INFER=0, GPU_OP_COPY_IN=1, GPU_OP_COPY_OUT=2, GPU_OP_BARRIER=3 } gpu_op_t;

typedef struct __attribute__((packed, aligned(64))) {
    uint32_t magic; gpu_op_t op; gpu_dtype_t dtype;
    uint8_t ndim; uint8_t _pad0;
    uint32_t shape[8];
    uint64_t offset; uint64_t size;
    uint64_t result_offset; uint64_t result_size;
    uint32_t seq; uint32_t flags;
} gpu_tensor_desc_t;

typedef struct __attribute__((aligned(64))) {
    uint32_t magic; uint32_t version; uint32_t depth; uint32_t _pad;
    volatile uint32_t head; volatile uint32_t tail;
    volatile uint32_t result_head; volatile uint32_t result_tail;
    uint8_t _pad2[32];
    gpu_tensor_desc_t slots[GPU_SHMEM_RING_DEPTH];
} gpu_shmem_ring_t;

static volatile int g_stop = 0;
static void sig_handler(int s) { (void)s; g_stop = 1; }

static void dispatch_tensor(gpu_tensor_desc_t *desc, void *payload_base) {
    printf("[gpu_shmem] dispatch seq=%u op=%d dtype=%d offset=%llu size=%llu\n",
           desc->seq, desc->op, desc->dtype,
           (unsigned long long)desc->offset, (unsigned long long)desc->size);

    /* In a real implementation, this would call:
     *   cudaMemcpy(d_buf, payload_base + desc->offset, desc->size, cudaMemcpyHostToDevice)
     *   torch dispatch...
     * For the prototype we just log the descriptor. */
    (void)payload_base;
}

int main(int argc, char **argv) {
    uint64_t phys_addr = 0;
    size_t   map_size  = GPU_SHMEM_BUF_SIZE;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pa") && i+1 < argc)
            phys_addr = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--size") && i+1 < argc)
            map_size = strtoull(argv[++i], NULL, 0);
    }

    if (!phys_addr) {
        fprintf(stderr, "Usage: %s --pa <phys_addr> [--size <bytes>]\n", argv[0]);
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }

    void *base = mmap(NULL, map_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, (off_t)phys_addr);
    if (base == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    gpu_shmem_ring_t *ring    = (gpu_shmem_ring_t *)base;
    void             *payload = (uint8_t *)base + GPU_SHMEM_PAYLOAD_OFF;

    /* Wait for seL4 producer to initialise the ring */
    printf("[gpu_shmem] waiting for ring initialisation at PA 0x%llx...\n",
           (unsigned long long)phys_addr);
    while (!g_stop && ring->magic != GPU_SHMEM_MAGIC)
        usleep(1000);

    if (g_stop) goto cleanup;

    printf("[gpu_shmem] ring ready (depth=%u). Polling for tensors...\n",
           ring->depth);

    while (!g_stop) {
        uint32_t head = ring->head;
        uint32_t tail = ring->tail;

        if (head == tail) {
            usleep(100); /* 100µs poll — replace with UIO wait in production */
            continue;
        }

        uint32_t slot = tail % GPU_SHMEM_RING_DEPTH;
        gpu_tensor_desc_t desc = ring->slots[slot];

        if (desc.magic != GPU_SHMEM_MAGIC) {
            ring->tail = tail + 1;
            continue;
        }

        /* Acknowledge dequeue */
        __sync_synchronize();
        ring->tail = tail + 1;
        __sync_synchronize();

        dispatch_tensor(&desc, payload);

        /* Write a trivial result back */
        gpu_tensor_desc_t result = desc;
        result.op = GPU_OP_COPY_OUT;
        result.result_offset = desc.offset;
        result.result_size   = desc.size;

        uint32_t rslot = (ring->result_head % (GPU_SHMEM_RING_DEPTH / 2)) + (GPU_SHMEM_RING_DEPTH / 2);
        ring->slots[rslot] = result;
        __sync_synchronize();
        ring->result_head++;
        __sync_synchronize();

        printf("[gpu_shmem] result queued for seq=%u\n", desc.seq);
    }

cleanup:
    printf("[gpu_shmem] shutting down\n");
    munmap(base, map_size);
    close(fd);
    return 0;
}
