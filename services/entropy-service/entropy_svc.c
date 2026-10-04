/*
 * services/entropy-service/entropy_svc.c — virtio-rng driver PD for agentOS
 *
 * entropy_pd is a DRIVER protection domain: it owns one virtio-rng host
 * device and nothing else. It is not a virtualizer -- there is no
 * multiplexing decision to make, because every caller gets independent
 * output and no caller's request affects another's.
 *
 * SCOPE OF WHAT THIS PROVIDES. See contracts/entropy_contract.h: this
 * service returns bytes produced by the virtio-rng device it owns and makes
 * no claim about statistical quality, entropy estimation, or cryptographic
 * suitability of that source. Under QEMU the backing source is the host's
 * RNG, which establishes nothing about a real board.
 *
 * Uses the shared virtio host transport (platform/virtio_host_transport.h)
 * for feature negotiation, status and queue setup -- the same transport
 * services/block-driver/virtio_blk.c uses. No separate MMIO access path.
 *
 * Polls rather than taking an IRQ: entropy has no latency requirement, and
 * polling avoids IRQ provisioning entirely. If the device is absent or
 * never reaches DRIVER_OK, this driver logs once and replies
 * AOS_ENTROPY_ERR_UNAVAILABLE to every request. It never spins or blocks
 * forever -- every poll loop below is bounded.
 *
 * ON THIS QEMU MACHINE, THE "MMIO FRAME" IS ORDINARY RAM, NOT A DEVICE.
 * QEMU `virt`'s virtio-mmio aperture is exactly 32 slots and all four are
 * already exclusively owned by other driver PDs (see
 * platform/include/platform/entropy_host_layout.h); there is no physical
 * address left in that aperture for entropy_pd. A physical address outside
 * any QEMU-modeled device or RAM region is NOT a safe substitute: reading
 * genuinely unbacked device-reserved physical memory was tried during this
 * task and reliably wedged the reading thread rather than delivering a
 * prompt fault -- exactly the hang this driver exists to avoid. So
 * entropy_mmio_vaddr here is backed by a private RAM frame the root task
 * allocates and maps the same way it maps the queue frame, not a `-device`
 * entry. Reading it is always safe (ordinary memory, no external-abort
 * risk) and, because real RAM near-never happens to start with the virtio
 * magic value, reliably exercises the same "no valid device" path real
 * hardware would present if a virtio-rng board header were wired and
 * powered off. This is a QEMU-virt-specific accommodation; a real board
 * would give entropy_pd actual device MMIO at this frame's physical
 * address instead.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "agentos.h"
#include "sel4_server.h"
#include "arch_barrier.h"
#include "contracts/entropy_contract.h"
#include <platform/entropy_host_layout.h>
#include <platform/virtio_host_transport.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Microkit setvar_vaddr symbols, written by the root task before init() on
 * boot flows that use that mechanism. This build's boot flow bakes the
 * mapping into a fixed VA directly (see main.c's entropy_pd MMIO/queue map
 * block), so both fall back to their compile-time host layout constants --
 * the same pattern virtio_blk.c uses for blk_mmio_vaddr.
 */
uintptr_t entropy_mmio_vaddr;
uintptr_t entropy_queue_vaddr;

/* log_drain_rings_vaddr required by log_drain_write() in agentos.h */
uintptr_t log_drain_rings_vaddr;

/* virtio device ID for an entropy source (virtio spec sect. 5.4: id 4). */
#define VIRTIO_ID_ENTROPY   4u

#define VIRTQ_DESC_F_WRITE  (1u << 1)

#define VIRTIO_STATUS_ACKNOWLEDGE  (1u << 0)
#define VIRTIO_STATUS_DRIVER       (1u << 1)
#define VIRTIO_STATUS_DRIVER_OK    (1u << 2)
#define VIRTIO_STATUS_FEATURES_OK  (1u << 3)
#define VIRTIO_STATUS_FAILED       (1u << 7)

/* Bounded polls. Never spin forever: an absent or wedged device must give
 * up and report AOS_ENTROPY_ERR_UNAVAILABLE, not hang the boot. */
#define ENTROPY_RESET_POLL_ITERS   100000u
#define ENTROPY_IO_POLL_ITERS      10000000u

/*
 * Single round-trip IPC reply capacity. sel4_msg_t carries SEL4_MSG_DATA_BYTES
 * (48) inline bytes total; this driver spends the first 8 on a status word
 * and a length word (the leading fields of aos_entropy_reply_t), leaving 40
 * for the random bytes one reply can actually carry.
 *
 * AOS_ENTROPY_MAX_BYTES (32, contracts/entropy_contract.h) is itself
 * wire-derived from this same 40-byte budget, so every contract-valid
 * request fits with margin; the AOS_ENTROPY_ERR_RANGE path below exists
 * for defense in depth (a future, larger AOS_ENTROPY_MAX_BYTES would hit
 * it immediately) rather than because it is reachable today.
 */
#define AOS_ENTROPY_WIRE_MAX_BYTES  (SEL4_MSG_DATA_BYTES - 8u)
_Static_assert(AOS_ENTROPY_WIRE_MAX_BYTES >= AOS_ENTROPY_MAX_BYTES,
               "every contract-valid request must fit in one sel4_msg_t reply");

/* One descriptor, one device-writable buffer: virtio-rng requests are a
 * single buffer the device fills (virtio spec sect. 5.4.6), no header, no
 * chained descriptors. */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} entropy_virtq_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[1];
} entropy_virtq_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} entropy_virtq_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    entropy_virtq_used_elem_t ring[1];
} entropy_virtq_used_t;

typedef struct {
    bool               initialized;
    aos_virtio_host_t  transport;
    uint64_t           queue_paddr; /* physical base of the queue/data frame */
    uint64_t           data_paddr;  /* physical addr of the rng data buffer  */
} entropy_device_t;

static entropy_device_t g_dev;

static volatile uint8_t *entropy_queue_mem(void)
{
    return (volatile uint8_t *)AGENTOS_ENTROPY_QUEUE_VA;
}
static volatile entropy_virtq_desc_t *entropy_desc(void)
{
    return (volatile entropy_virtq_desc_t *)(entropy_queue_mem() + AGENTOS_ENTROPY_DESC_OFF);
}
static volatile entropy_virtq_avail_t *entropy_avail(void)
{
    return (volatile entropy_virtq_avail_t *)(entropy_queue_mem() + AGENTOS_ENTROPY_AVAIL_OFF);
}
static volatile entropy_virtq_used_t *entropy_used(void)
{
    return (volatile entropy_virtq_used_t *)(entropy_queue_mem() + AGENTOS_ENTROPY_USED_OFF);
}
static volatile uint8_t *entropy_data(void)
{
    return entropy_queue_mem() + AGENTOS_ENTROPY_DATA_OFF;
}

/*
 * entropy_device_init() — bind the virtio-rng transport and bring the
 * device to DRIVER_OK, or give up and leave g_dev.initialized false.
 *
 * Every wait in here is a bounded poll. A device that never acknowledges
 * reset, never accepts features, or never accepts the queue is reported
 * once and left uninitialised -- callers get AOS_ENTROPY_ERR_UNAVAILABLE,
 * not a hung boot.
 */
static void entropy_device_init(void)
{
    g_dev.initialized = false;

    if (entropy_mmio_vaddr == 0u)
        entropy_mmio_vaddr = AGENTOS_HOST_ENTROPY_MMIO_VA;
    if (entropy_queue_vaddr == 0u)
        entropy_queue_vaddr = AGENTOS_ENTROPY_QUEUE_VA;

    const agentos_entropy_shared_meta_t *shared =
        (const agentos_entropy_shared_meta_t *)entropy_queue_vaddr;
    if (shared->magic != AGENTOS_ENTROPY_SHARED_MAGIC ||
        shared->version != AGENTOS_ENTROPY_SHARED_VERSION ||
        shared->size != AGENTOS_ENTROPY_QUEUE_SIZE) {
        log_drain_write(17, 17,
            "[entropy_pd] WARNING: queue frame metadata invalid; device unavailable\n");
        return;
    }
    g_dev.queue_paddr = shared->paddr;
    g_dev.data_paddr  = shared->paddr + AGENTOS_ENTROPY_DATA_OFF;

    aos_virtio_host_t *t = &g_dev.transport;
    if (!aos_virtio_host_mmio(t, entropy_mmio_vaddr, 0x200u, VIRTIO_ID_ENTROPY)) {
        log_drain_write(17, 17,
            "[entropy_pd] WARNING: virtio-rng device absent; serving AOS_ENTROPY_ERR_UNAVAILABLE\n");
        return;
    }

    /* virtio spec sect. 3.1.1 step 1 — reset, then confirm it took. */
    aos_virtio_host_set_status(t, 0);
    unsigned reset_poll;
    for (reset_poll = 0; reset_poll < ENTROPY_RESET_POLL_ITERS; reset_poll++) {
        if (!aos_virtio_host_status(t)) break;
    }
    if (reset_poll == ENTROPY_RESET_POLL_ITERS) {
        log_drain_write(17, 17, "[entropy_pd] WARNING: device did not reset; unavailable\n");
        return;
    }

    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE;
    aos_virtio_host_set_status(t, status);
    status |= VIRTIO_STATUS_DRIVER;
    aos_virtio_host_set_status(t, status);

    /* virtio-rng has no device-specific feature bits; still negotiate
     * VIRTIO_F_VERSION_1 (feature word 1, bit 0), which modern virtio
     * devices require. */
    (void)aos_virtio_host_features(t, 0);
    aos_virtio_host_set_features(t, 0, 0u);
    if (!(aos_virtio_host_features(t, 1) & 1u)) {
        log_drain_write(17, 17, "[entropy_pd] WARNING: device rejected VERSION_1; unavailable\n");
        aos_virtio_host_set_status(t, status | VIRTIO_STATUS_FAILED);
        return;
    }
    aos_virtio_host_set_features(t, 1, 1u);

    status |= VIRTIO_STATUS_FEATURES_OK;
    aos_virtio_host_set_status(t, status);
    if (!(aos_virtio_host_status(t) & VIRTIO_STATUS_FEATURES_OK)) {
        log_drain_write(17, 17, "[entropy_pd] WARNING: device rejected feature set; unavailable\n");
        aos_virtio_host_set_status(t, status | VIRTIO_STATUS_FAILED);
        return;
    }

    /* Zero the descriptor/avail/used region and the data buffer this driver
     * actually uses. Metadata below AGENTOS_ENTROPY_DESC_OFF is untouched. */
    volatile uint8_t *qm = entropy_queue_mem();
    uint32_t zero_end = AGENTOS_ENTROPY_DATA_OFF + AOS_ENTROPY_WIRE_MAX_BYTES;
    for (uint32_t off = AGENTOS_ENTROPY_DESC_OFF; off < zero_end; off++) {
        qm[off] = 0;
    }
    ARCH_WMB();

    uint64_t qpa = g_dev.queue_paddr;
    entropy_avail()->flags = 1u; /* VIRTQ_AVAIL_F_NO_INTERRUPT: we poll */
    ARCH_WMB();
    if (!aos_virtio_host_queue(t, 0u, 1u,
                               qpa + AGENTOS_ENTROPY_DESC_OFF,
                               qpa + AGENTOS_ENTROPY_AVAIL_OFF,
                               qpa + AGENTOS_ENTROPY_USED_OFF)) {
        log_drain_write(17, 17, "[entropy_pd] WARNING: queue setup rejected; unavailable\n");
        aos_virtio_host_set_status(t, status | VIRTIO_STATUS_FAILED);
        return;
    }

    status |= VIRTIO_STATUS_DRIVER_OK;
    aos_virtio_host_set_status(t, status);

    g_dev.initialized = true;
    log_drain_write(17, 17, "[entropy_pd] virtio-rng device ready\n");
}

/*
 * entropy_device_read() — fill out[0..len) from the device.
 *
 * Bounded poll on the used ring (ENTROPY_IO_POLL_ITERS); on timeout or a
 * short device completion this returns false and the caller reports
 * AOS_ENTROPY_ERR_UNAVAILABLE rather than retrying forever.
 */
static bool entropy_device_read(uint8_t *out, uint32_t len)
{
    volatile entropy_virtq_desc_t *desc = entropy_desc();
    volatile entropy_virtq_avail_t *avail = entropy_avail();
    volatile entropy_virtq_used_t *used = entropy_used();
    volatile uint8_t *dma = entropy_data();

    desc[0].addr  = g_dev.data_paddr;
    desc[0].len   = len;
    desc[0].flags = VIRTQ_DESC_F_WRITE;
    desc[0].next  = 0;

    uint16_t used_idx_before = used->idx;
    uint16_t avail_idx = avail->idx;
    avail->ring[avail_idx % 1u] = 0;
    ARCH_WMB();
    avail->idx = (uint16_t)(avail_idx + 1u);
    ARCH_WMB();

    if (!aos_virtio_host_notify(&g_dev.transport)) {
        return false;
    }

    uint32_t iters = 0;
    while (used->idx == used_idx_before) {
        ARCH_MB();
        if (++iters >= ENTROPY_IO_POLL_ITERS) {
            log_drain_write(17, 17, "[entropy_pd] ERROR: I/O timeout\n");
            return false;
        }
    }
    ARCH_MB();

    uint32_t written = used->ring[0].len;
    if (written < len) {
        log_drain_write(17, 17, "[entropy_pd] ERROR: short device completion\n");
        return false;
    }
    for (uint32_t i = 0; i < len; i++) out[i] = dma[i];
    return true;
}

/*
 * entropy_h_get() — serve MSG_ENTROPY_GET.
 *
 * Wire request: data[0..12) is aos_entropy_req_t (version, length, reserved).
 * Wire reply on success: data[0..4)=AOS_ENTROPY_OK, data[4..8)=length,
 * data[8..8+length)=random bytes, rep->length = 8 + length. On any failure
 * (bad request, device absent, device wedged): data[0..4)=status code,
 * data[4..8)=0, rep->length = 8. The handler always returns SEL4_ERR_OK:
 * AOS_ENTROPY_* is an application-level status in the payload, not a
 * transport error.
 */
static uint32_t entropy_h_get(sel4_badge_t badge, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    aos_entropy_req_t creq = {0};
    if (req->length >= sizeof(creq)) {
        creq.version  = msg_u32(req, 0);
        creq.length   = msg_u32(req, 4);
        creq.reserved = msg_u32(req, 8);
    }

    int status = aos_entropy_validate_req(&creq);
    if (status == AOS_ENTROPY_OK && creq.length > AOS_ENTROPY_WIRE_MAX_BYTES) {
        status = AOS_ENTROPY_ERR_RANGE;
    }
    if (status == AOS_ENTROPY_OK && !g_dev.initialized) {
        status = AOS_ENTROPY_ERR_UNAVAILABLE;
    }

    if (status != AOS_ENTROPY_OK) {
        rep_u32(rep, 0, (uint32_t)status);
        rep_u32(rep, 4, 0u);
        rep->length = 8u;
        return SEL4_ERR_OK;
    }

    uint8_t buf[AOS_ENTROPY_WIRE_MAX_BYTES];
    if (!entropy_device_read(buf, creq.length)) {
        /* A device that fails an in-flight read is treated as gone: stop
         * retrying it every call and report unavailable from here on. */
        g_dev.initialized = false;
        rep_u32(rep, 0, (uint32_t)AOS_ENTROPY_ERR_UNAVAILABLE);
        rep_u32(rep, 4, 0u);
        rep->length = 8u;
        return SEL4_ERR_OK;
    }

    rep_u32(rep, 0, (uint32_t)AOS_ENTROPY_OK);
    rep_u32(rep, 4, creq.length);
    for (uint32_t i = 0; i < creq.length; i++) {
        rep->data[8u + i] = buf[i];
    }
    rep->length = 8u + creq.length;
    return SEL4_ERR_OK;
}

void entropy_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    agentos_log_boot("entropy_pd");
    entropy_device_init();

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, MSG_ENTROPY_GET, entropy_h_get, (void *)0);
    sel4_server_run(&srv);
}

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep) { entropy_main(my_ep, ns_ep); }
