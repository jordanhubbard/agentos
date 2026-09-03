/*
 * agentOS inspect snapshot ABI
 *
 * Read-only view of memory, threads (protection domains), and hardware
 * attributes. Host-testable: no seL4 headers. A later inspect PD copies
 * the same packed structs over IPC; it does not own device frames or IRQs.
 *
 * This is not a UI. The formatter emits structured key=value lines for a
 * client (guest Hermes, or any serial_virt consumer).
 */

#ifndef AOS_PLATFORM_INSPECT_H
#define AOS_PLATFORM_INSPECT_H

#include <stddef.h>
#include <stdint.h>

#define AOS_INSPECT_VERSION            1u
#define AOS_INSPECT_MAX_THREADS        32u
#define AOS_INSPECT_NAME_LEN           32u

#define AOS_INSPECT_ARCH_UNKNOWN       0u
#define AOS_INSPECT_ARCH_AARCH64       1u
#define AOS_INSPECT_ARCH_X86_64        2u
#define AOS_INSPECT_ARCH_RISCV64       3u

#define AOS_INSPECT_THR_UNKNOWN        0u
#define AOS_INSPECT_THR_RUNNING        1u
#define AOS_INSPECT_THR_BLOCKED        2u
#define AOS_INSPECT_THR_IDLE           3u

#define AOS_INSPECT_FLAG_PARTIAL       1u

#define AOS_INSPECT_OK                 0
#define AOS_INSPECT_ERR_NULL          (-1)
#define AOS_INSPECT_ERR_VERSION       (-2)
#define AOS_INSPECT_ERR_TRUNC         (-3)
#define AOS_INSPECT_ERR_TOO_MANY      (-4)
#define AOS_INSPECT_ERR_NOT_FOUND     (-5)

typedef struct __attribute__((packed)) aos_inspect_memory {
    uint64_t ut_total_bytes;
    uint64_t ut_used_bytes;
    uint64_t guest_ram_bytes;
    uint32_t pd_count;
    uint32_t reserved;
} aos_inspect_memory_t;

typedef struct __attribute__((packed)) aos_inspect_hardware {
    uint32_t arch;
    uint32_t virtio_net_virq;
    uint64_t uart_pa;
    uint64_t gic_dist_pa;
    uint64_t virtio_net_ipa;
} aos_inspect_hardware_t;

typedef struct __attribute__((packed)) aos_inspect_thread {
    uint32_t pd_index;
    uint32_t prio;
    uint32_t state;
    uint8_t  name[AOS_INSPECT_NAME_LEN];
} aos_inspect_thread_t;

typedef struct __attribute__((packed)) aos_inspect_snapshot {
    uint32_t version;
    uint32_t flags;
    aos_inspect_memory_t mem;
    aos_inspect_hardware_t hw;
    uint32_t thread_count;
    uint32_t reserved;
    aos_inspect_thread_t threads[AOS_INSPECT_MAX_THREADS];
} aos_inspect_snapshot_t;

/*
 * Observation injected by the root task (or a host test). Not a live seL4
 * query — the filler only copies and validates.
 */
typedef struct aos_inspect_view {
    uint64_t ut_total_bytes;
    uint64_t ut_used_bytes;
    uint64_t guest_ram_bytes;
    uint32_t arch;
    uint32_t virtio_net_virq;
    uint64_t uart_pa;
    uint64_t gic_dist_pa;
    uint64_t virtio_net_ipa;
    uint32_t thread_count;
    aos_inspect_thread_t threads[AOS_INSPECT_MAX_THREADS];
} aos_inspect_view_t;

int aos_inspect_fill(aos_inspect_snapshot_t *snap, const aos_inspect_view_t *view);
int aos_inspect_format(const aos_inspect_snapshot_t *snap, char *buf, size_t buflen);
int aos_inspect_thread_by_name(const aos_inspect_snapshot_t *snap,
                               const char *name,
                               const aos_inspect_thread_t **out);

#endif /* AOS_PLATFORM_INSPECT_H */
