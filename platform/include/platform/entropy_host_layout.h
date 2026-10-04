/*
 * Host virtio-rng MMIO/queue layout.
 *
 * ── Why entropy_pd has no physical virtio-mmio slot on QEMU virt ───────────
 * QEMU's `virt` machine hard-caps virtio-mmio at 32 slots, 0x0a000000 +
 * slot*0x200, confirmed via `qemu-system-aarch64 -machine virt,help`
 * (`virtio-mmio-transports` rejects anything above 32) and `info mtree`
 * (the aperture ends at 0x0a004000). Root retypes device untypeds at 4 KiB
 * (one-page) granularity, and each slot is only 0x200 bytes apart, so
 * eight slots share one retypeable page: slots 0-7 (page 0x0a000000) are
 * the root task's generic virtio-mmio probe frame plus cc_pd's slot 2;
 * slots 8-15 (page 0x0a001000) are virtio_blk's primary medium; slots
 * 16-23 (page 0x0a002000) are net_pd's; slots 24-31 (page 0x0a003000) are
 * virtio_blk's secondary medium. All four pages -- every slot QEMU
 * exposes -- are already exclusively owned before entropy_pd exists. The
 * brief's proposed slot 4 (0x0a000800) is inside the first of those four
 * pages; a boot attempt confirmed it fails retype with
 * seL4_InvalidArgument for exactly this reason. Giving entropy_pd any of
 * the 32 would mean two PDs mapping one physical frame (the TCB invariant
 * 1 violation this project exists to prevent), and QEMU will not create a
 * 33rd bus. A real fix needs either shrinking an existing driver's
 * footprint or a virtio-pci transport for entropy; both are out of this
 * task's scope -- see docs/TCB.md and the task report for the call this
 * makes instead.
 *
 * ── Why AGENTOS_HOST_ENTROPY_MMIO_VA is backed by RAM, not a physical MMIO
 *    address outside that aperture ──────────────────────────────────────
 * The obvious next idea -- retype a page from the same device-untyped
 * region (0x0a000000-0x0c000000) but past the 32-slot array, e.g.
 * 0x0a004000 -- was tried and is NOT safe. The retype and mapping succeed
 * (entropy_pd does genuinely, uniquely own that frame), but the physical
 * address corresponds to nothing QEMU models at all -- not a device, not
 * RAM. A guest read of truly unbacked physical memory in this environment
 * does not deliver a prompt, catchable fault the way an MMU translation or
 * permission fault does; it reliably wedges the reading thread instead,
 * with no further progress and no log output -- confirmed by instrumenting
 * entropy_device_init() and observing it hang immediately inside
 * aos_virtio_host_mmio()'s first register read, every time. That is
 * exactly the hang this driver exists to avoid, and worse than having no
 * device at all: a `-device` that was simply never attached produces a
 * clean, fast "wrong magic" rejection; genuinely unbacked physical memory
 * does not.
 *
 * So AGENTOS_HOST_ENTROPY_MMIO_VA is backed by a private RAM frame the
 * root task allocates the same way it allocates the queue frame below
 * (ordinary untyped memory, not a device untyped, via
 * allocate_entropy_mmio_probe() in main.c) and maps at this VA. Reading
 * ordinary RAM is always safe -- no external-abort risk, no hang -- and
 * because real memory essentially never happens to start with the virtio
 * magic value, the driver's unmodified handshake code reliably finds "no
 * valid device" here and reports AOS_ENTROPY_ERR_UNAVAILABLE, the same
 * outcome a real but powered-off or absent device would produce. This is
 * a QEMU-virt-specific accommodation for the fact that no real device
 * register bank is reachable on this machine; a real board would give
 * entropy_pd actual device MMIO at this frame's physical address instead,
 * and the driver code does not need to change to use it.
 *
 * entropy_pd also owns a second, private 4 KiB frame for its virtqueue and
 * data buffer. The root task allocates that frame at boot, writes its own
 * physical address into agentos_entropy_shared_meta_t at the frame's first
 * bytes, then maps it read-write into entropy_pd at
 * AGENTOS_ENTROPY_QUEUE_VA -- the same "write metadata, then hand over the
 * frame" pattern blk_host_layout.h and net_host_layout.h use so a driver
 * never has to assume a virtual address is also a DMA address.
 *
 * Only entropy_pd receives either mapping; no other PD maps either
 * physical frame (TCB invariant 1: one owner per device frame).
 */
#ifndef AOS_PLATFORM_ENTROPY_HOST_LAYOUT_H
#define AOS_PLATFORM_ENTROPY_HOST_LAYOUT_H

#include <stdint.h>

/* Virtual addresses only: both frames are allocated from ordinary untyped
 * memory at boot (see allocate_entropy_mmio_probe() / allocate_entropy_queue()
 * in main.c), so neither has a meaningful fixed physical address to name
 * here -- unlike blk/net, which bind to real host device PAs. */
#define AGENTOS_HOST_ENTROPY_MMIO_VA      0x06400000UL

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
