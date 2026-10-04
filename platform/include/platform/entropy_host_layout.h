/*
 * Host virtio-rng queue layout.
 *
 * entropy_pd has no device frame on QEMU virt at all -- not a real
 * virtio-mmio slot (all 32 QEMU exposes are already owned by other driver
 * PDs) and not a RAM frame standing in for one either (a RAM frame cannot
 * honestly be said to satisfy "one owner per device frame": it is not a
 * device frame, so the invariant would be vacuously true). The root task
 * provisions no MMIO frame for entropy_pd on this machine; see the header
 * comment in services/entropy-service/entropy_svc.c and docs/TCB.md for
 * the full account of why, including the retype-collision and
 * unbacked-physical-memory findings from building and testing this
 * driver.
 *
 * What remains real, and is defined here, is entropy_pd's private
 * virtqueue and data buffer: a 4 KiB frame the root task allocates at
 * boot (ordinary untyped memory, not a device untyped -- it has no fixed
 * physical address to name here, unlike blk/net's real host device
 * frames), writes its own physical address into
 * agentos_entropy_shared_meta_t at the frame's first bytes, then maps
 * read-write into entropy_pd at AGENTOS_ENTROPY_QUEUE_VA -- the same
 * "write metadata, then hand over the frame" pattern blk_host_layout.h
 * and net_host_layout.h use so a driver never has to assume a virtual
 * address is also a DMA address. This frame exists so the driver's
 * virtio-rng handshake code is ready to drive a real device the moment
 * one is reachable (a real board, or a future QEMU configuration with a
 * free slot); only entropy_pd maps it.
 */
#ifndef AOS_PLATFORM_ENTROPY_HOST_LAYOUT_H
#define AOS_PLATFORM_ENTROPY_HOST_LAYOUT_H

#include <stdint.h>

#define AGENTOS_ENTROPY_QUEUE_VA          0x06500000UL
#define AGENTOS_ENTROPY_QUEUE_SIZE        0x1000UL /* one 4 KiB page */

#define AGENTOS_ENTROPY_SHARED_MAGIC      0x414F5345u /* "AOSE" */
#define AGENTOS_ENTROPY_SHARED_VERSION    1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint64_t paddr;
    uint32_t size;
} agentos_entropy_shared_meta_t;

/*
 * Layout inside the 4 KiB queue/data frame. The metadata header occupies
 * the first 24 bytes; the remaining regions get generous alignment so a
 * single descriptor chain (one descriptor: the rng read buffer) and its
 * backing data never overlap.
 */
#define AGENTOS_ENTROPY_DESC_OFF          0x40u
#define AGENTOS_ENTROPY_AVAIL_OFF         0x100u
#define AGENTOS_ENTROPY_USED_OFF          0x200u
#define AGENTOS_ENTROPY_DATA_OFF          0x300u

#endif /* AOS_PLATFORM_ENTROPY_HOST_LAYOUT_H */
