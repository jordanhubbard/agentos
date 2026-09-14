#ifndef AOS_LOG_RING_H
#define AOS_LOG_RING_H
#include <stdint.h>
#include <stddef.h>
#define AOS_LOG_MAGIC UINT32_C(0xc0de4d55)
#define AOS_LOG_CONFIG_MAGIC UINT32_C(0x4c4f4732)
#define AOS_LOG_VERSION 2u
#define AOS_LOG_PAGE 4096u
#define AOS_LOG_DATA 4080u
#define AOS_LOG_CLIENTS 64u
#define AOS_LOG_CONFIG_VA UINT64_C(0x1000a000)
#define AOS_LOG_CLIENT_VA UINT64_C(0x1000b000)
#define AOS_LOG_SERVER_VA UINT64_C(0x2b000000)
#define AOS_LOG_NOTIFY_CAP 31u
#define AOS_LOG_WAKE (UINT64_C(1) << 63)
#define AOS_LOG_DISABLED 0u
#define AOS_LOG_CLIENT 1u
#define AOS_LOG_SERVER 2u
typedef struct {
    uint32_t magic, pd_id, head, tail;
    uint8_t data[AOS_LOG_DATA];
} aos_log_ring_t;
typedef struct { uint32_t enabled, service_id; char name[32]; } aos_log_identity_t;
typedef struct {
    uint32_t magic, version, role, slot, count, reserved[3];
    aos_log_identity_t clients[AOS_LOG_CLIENTS];
} aos_log_config_t;
_Static_assert(sizeof(aos_log_ring_t) == AOS_LOG_PAGE, "one log ring per page");
_Static_assert(sizeof(aos_log_config_t) <= AOS_LOG_PAGE, "log config fits page");

/* Single producer, single consumer. Best effort: retain queued bytes, drop
 * new bytes beyond available space. Snapshot and bound untrusted cursors.
 * No caller-selected slot, allocation, IPC call or unbounded string scan. */
static inline uint32_t aos_log_ring_write(aos_log_ring_t *ring, const char *text)
{
    if (!ring || !text || ring->magic != AOS_LOG_MAGIC) return 0;
    uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    if (head >= AOS_LOG_DATA || tail >= AOS_LOG_DATA) return 0;
    uint32_t n = 0;
    while (n < AOS_LOG_DATA - 1 && text[n]) {
        uint32_t next = (head + 1) % AOS_LOG_DATA;
        if (next == tail) break;
        ring->data[head] = (uint8_t)text[n++];
        head = next;
    }
    __atomic_store_n(&ring->head, head, __ATOMIC_RELEASE);
    return n;
}
#endif
