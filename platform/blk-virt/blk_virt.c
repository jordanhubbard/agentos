/*
 * blk_virt PD — block virtualizer (docs/TCB.md target: `blk_virt`).
 *
 * The only block mux.  Owns no device frame and no IRQ.  Sits between the
 * guest-facing sDDF queues in the shared block region (AOS_BLK_SHMEM_VA, one
 * AOS_BLK_CLIENT_STRIDE per guest VMM slot, produced/consumed by the VMM's
 * emulated virtio-blk) and virtio_blk, the host block driver PD.
 *
 * Contract: include/contracts/blk_virt_contract.h.
 *
 * Data path (per attached client):
 *   request    VMM enqueues on its request queue, NBSends KICK  ->  blk_virt
 *              dequeues, moves the data cells through virtio_blk's bounded
 *              per-media DMA window by seL4 Call (chunked to the window's
 *              sector limit), enqueues the response, NBSends RESP_READY to
 *              the owning VMM, which completes the guest virtq descriptor.
 *   attach     one Call: blk_virt probes the media (OP_INFO) and fills the
 *              client's sDDF storage_info; with no host media it serves a
 *              per-client RAM disk instead so the emulated-scope proof still
 *              completes guest requests.
 *
 * blk_virt is the only PD that holds the virtio_blk endpoint or maps the DMA
 * window, so two guests can no longer race each other in that window: their
 * requests are serialised here.
 *
 * Notifications are NBSend on endpoints and can be dropped when the target
 * is not blocked in Recv.  Every drop is recoverable: the VMM re-kicks on
 * the next guest exit while its request queue is non-empty and our
 * req_consumer_signalled word is 0, and the VMM drains its response queue
 * after every guest exit, so a dropped RESP_READY only costs latency.
 */

#include "agentos.h"
#include "sel4_ipc.h"
#include "serial_log.h"
#include "system_desc.h"
#include "nameserver.h"
#include <contracts/blk_virt_contract.h>
#include <platform/blk_layout.h>
#include <platform/blk_host_layout.h>
#include <platform/blk_virt_pump.h>

_Static_assert(sizeof(blk_virt_attach_req_t) == 16u,
               "blk_virt ATTACH request wire size");
_Static_assert(sizeof(blk_virt_attach_reply_t) == 24u,
               "blk_virt ATTACH reply wire size");
_Static_assert(AOS_BLK_SHMEM_VA != AGENTOS_BLK_SHARED_VA,
               "guest block queues and the driver DMA window are distinct frames");
_Static_assert(AOS_BLK_TRANSFER_SIZE % AOS_HOST_BLK_SECTOR_SIZE == 0u,
               "sDDF transfer unit must be whole host sectors");

/* Unmapped: log_drain_write falls back to the (release-silent) debug putc.
 * Visible diagnostics go through serial_pd via serial_log_t. */
uintptr_t log_drain_rings_vaddr;

typedef struct {
    uint8_t               attached;
    uint8_t               hw;           /* requests go to virtio_blk (else RAM) */
    uint8_t               pumped_marked;
    uint8_t               read_marked;
    uint32_t              client_id;
    uint32_t              media_id;
    seL4_CPtr             vmm_ep;       /* owning VMM listen EP (RESP_READY) */
    uint32_t              requests;
    uint32_t              responses;
    aos_blk_virt_client_t q;
    aos_blk_virt_t        virt;         /* one pump instance per client */
} bv_client_t;

static bv_client_t  g_clients[AOS_BLK_MAX_CLIENTS];
static uint8_t      g_ram_disk[AOS_BLK_MAX_CLIENTS][AOS_BLK_DISK_BYTES]
                        __attribute__((aligned(4096)));
static serial_log_t g_log = { .ep = PD_CNODE_SLOT_SERIAL_EP };

/* ── diagnostics ────────────────────────────────────────────────────────── */

static void bv_puts(const char *s)
{
    serial_log_puts(&g_log, s);
}

static void bv_dec(uint64_t v)
{
    char buf[24];
    int i = 23;

    buf[i] = '\0';
    if (v == 0u) {
        buf[--i] = '0';
    }
    while (v > 0u && i > 0) {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    bv_puts(&buf[i]);
}

static uint32_t rd32(const uint8_t *p, uint32_t off)
{
    return (uint32_t)p[off] |
           ((uint32_t)p[off + 1u] << 8) |
           ((uint32_t)p[off + 2u] << 16) |
           ((uint32_t)p[off + 3u] << 24);
}

static void wr32(uint8_t *p, uint32_t off, uint32_t v)
{
    p[off] = (uint8_t)v;
    p[off + 1u] = (uint8_t)(v >> 8);
    p[off + 2u] = (uint8_t)(v >> 16);
    p[off + 3u] = (uint8_t)(v >> 24);
}

static void bv_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0u; i < n; i++) {
        dst[i] = src[i];
    }
}

static void bv_fence(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void register_with_nameserver(seL4_CPtr ns_ep)
{
    static const char name[] = "blk_virt";
    sel4_msg_t req = {0}, rep = {0};

    if (!ns_ep) {
        return;
    }
    req.opcode = (uint32_t)OP_NS_REGISTER;
    wr32(req.data, 0u, 0u);
    wr32(req.data, 4u, 0u);
    wr32(req.data, 8u, 0u);
    wr32(req.data, 12u, 1u);
    for (uint32_t i = 0u; i < sizeof(name); i++) {
        req.data[16u + i] = (uint8_t)name[i];
    }
    req.length = 16u + (uint32_t)sizeof(name);
    sel4_call(ns_ep, &req, &rep);
}

/* ── virtio_blk driver client: chunked I/O through the per-media DMA window ── */

static uint8_t *host_dma(uint32_t media_id)
{
    return (uint8_t *)(AGENTOS_BLK_SHARED_VA +
                       AGENTOS_BLK_MEDIA_DMA_OFF(media_id) +
                       AGENTOS_BLK_SHARED_DMA_DATA_OFF);
}

/*
 * One driver Call.  Wire format is virtio_blk's canonical block contract:
 * MR0 op, MR1 length (20), MR2 = op | sector_lo<<32, MR3 = sector_hi |
 * count<<32, MR4 media.  INFO replies capacity in sectors and
 * AOS_HOST_BLK_INFO_* flags (MR4: READ_ONLY mirrors the media's VIRTIO_BLK_F_RO).
 */
static uint32_t host_blk_call(uint32_t media_id, uint32_t op, uint64_t sector,
                              uint32_t count, uint64_t *capacity,
                              bool *read_only)
{
    seL4_Word payload0 = (seL4_Word)op |
                         ((seL4_Word)(uint32_t)sector << 32);
    seL4_Word payload1 = (seL4_Word)(uint32_t)(sector >> 32) |
                         ((seL4_Word)count << 32);
    seL4_MessageInfo_t tag;
    seL4_MessageInfo_t reply;

    seL4_SetMR(0, op);
    seL4_SetMR(1, 20u);
    seL4_SetMR(2, payload0);
    seL4_SetMR(3, payload1);
    seL4_SetMR(4, (seL4_Word)media_id);
    seL4_SetMR(5, 0u);
    seL4_SetMR(6, 0u);
    seL4_SetMR(7, 0u);
    tag = seL4_MessageInfo_new(op, 0u, 0u, 8u);
    reply = seL4_Call((seL4_CPtr)PD_CNODE_SLOT_VIRTIO_BLK_EP, tag);
    if (seL4_MessageInfo_get_length(reply) < 3u ||
        seL4_GetMR(1) < 4u) {
        return AOS_HOST_BLK_ERR_NODEV;
    }

    payload0 = seL4_GetMR(2);
    if (capacity && (uint32_t)payload0 == AOS_HOST_BLK_OK &&
        seL4_GetMR(1) >= 16u &&
        seL4_MessageInfo_get_length(reply) >= 4u) {
        payload1 = seL4_GetMR(3);
        *capacity = ((uint64_t)(uint32_t)payload1 << 32) |
                    (uint64_t)(uint32_t)(payload0 >> 32);
    }
    if (read_only && (uint32_t)payload0 == AOS_HOST_BLK_OK &&
        seL4_GetMR(1) >= 20u && seL4_MessageInfo_get_length(reply) >= 5u) {
        *read_only = ((uint32_t)seL4_GetMR(4) &
                      AOS_HOST_BLK_INFO_READ_ONLY) != 0u;
    }
    return (uint32_t)payload0;
}

static uint32_t host_blk_transfer(uint32_t media_id, uint32_t op,
                                  uint64_t sector, uint8_t *client_data,
                                  uint32_t nbytes)
{
    uint8_t *dma = host_dma(media_id);
    uint32_t sectors_left = nbytes / AOS_HOST_BLK_SECTOR_SIZE;
    uint32_t byte_offset = 0u;
    uint32_t max_sectors = AGENTOS_BLK_MEDIA_DMA_MAX_SECTORS(media_id);

    while (sectors_left > 0u) {
        uint32_t sectors = sectors_left;
        uint32_t bytes;
        uint32_t rc;

        if (sectors > max_sectors) {
            sectors = max_sectors;
        }
        bytes = sectors * AOS_HOST_BLK_SECTOR_SIZE;
        if (op == AOS_HOST_BLK_OP_WRITE) {
            bv_copy(dma, client_data + byte_offset, bytes);
            bv_fence();
        }
        rc = host_blk_call(media_id, op, sector, sectors, 0, 0);
        if (rc != AOS_HOST_BLK_OK) {
            return rc;
        }
        if (op == AOS_HOST_BLK_OP_READ) {
            bv_fence();
            bv_copy(client_data + byte_offset, dma, bytes);
        }
        sector += sectors;
        sectors_left -= sectors;
        byte_offset += bytes;
    }
    return AOS_HOST_BLK_OK;
}

/* Pump backend: one sDDF request against the client's host media. */
static aos_blk_resp_status_t host_blk_backend(
    void *ctx, aos_blk_virt_client_t *client, const aos_blk_req_t *req)
{
    bv_client_t *c = (bv_client_t *)ctx;
    uint32_t nbytes = (uint32_t)req->count * AOS_BLK_TRANSFER_SIZE;
    uint64_t data_end = req->io_or_offset + (uint64_t)nbytes;
    uint64_t sector = req->block_number *
                      (AOS_BLK_TRANSFER_SIZE / AOS_HOST_BLK_SECTOR_SIZE);
    uint32_t sectors = (uint32_t)req->count *
                       (AOS_BLK_TRANSFER_SIZE / AOS_HOST_BLK_SECTOR_SIZE);
    uint32_t rc;

    c->requests++;
    if (data_end > AOS_BLK_DATA_BYTES) {
        bv_puts("[blk_virt] request exceeds the client data cells\n");
        return AOS_BLK_RESP_ERR_INVALID_PARAM;
    }

    switch (req->code) {
    case AOS_BLK_REQ_READ:
        rc = host_blk_transfer(c->media_id, AOS_HOST_BLK_OP_READ, sector,
                               client->data + (uint32_t)req->io_or_offset,
                               nbytes);
        if (rc == AOS_HOST_BLK_OK && !c->read_marked) {
            c->read_marked = 1u;
            bv_puts("[blk_virt] host-media read sector=");
            bv_dec(sector);
            bv_puts(" count=");
            bv_dec(sectors);
            bv_puts(" client=");
            bv_dec(c->client_id);
            bv_puts("\n");
        }
        break;
    case AOS_BLK_REQ_WRITE:
        rc = host_blk_transfer(c->media_id, AOS_HOST_BLK_OP_WRITE, sector,
                               client->data + (uint32_t)req->io_or_offset,
                               nbytes);
        break;
    case AOS_BLK_REQ_FLUSH:
    case AOS_BLK_REQ_BARRIER:
        rc = host_blk_call(c->media_id, AOS_HOST_BLK_OP_FLUSH, 0u, 0u, 0, 0);
        break;
    default:
        return AOS_BLK_RESP_ERR_INVALID_PARAM;
    }

    if (rc == AOS_HOST_BLK_OK) {
        return AOS_BLK_RESP_OK;
    }
    bv_puts("[blk_virt] host request failed media=");
    bv_dec(c->media_id);
    bv_puts(" rc=");
    bv_dec(rc);
    bv_puts("\n");
    if (rc == AOS_HOST_BLK_ERR_NODEV) {
        return AOS_BLK_RESP_ERR_NO_DEVICE;
    }
    if (rc == AOS_HOST_BLK_ERR_OOB) {
        return AOS_BLK_RESP_ERR_INVALID_PARAM;
    }
    return AOS_BLK_RESP_ERR_IO;
}

/* ── events to the VMMs ─────────────────────────────────────────────────── */

static void bv_notify_vmm(const bv_client_t *c)
{
    seL4_MessageInfo_t event =
        seL4_MessageInfo_new(BLK_VIRT_EVENT_RESP_READY, 0u, 0u, 0u);

    if (c->vmm_ep != 0u) {
        seL4_NBSend(c->vmm_ep, event);
    }
}

static seL4_CPtr vmm_ep_for_slot(uint32_t vmm_slot)
{
#if defined(AGENTOS_GUEST_PRIMARY)
    if (vmm_slot == BLK_VIRT_VMM_SLOT_PRIMARY) {
        return (seL4_CPtr)PD_CNODE_SLOT_GUEST_VMM_PRIMARY_EP;
    }
#endif
#if defined(AGENTOS_GUEST_SECONDARY)
    if (vmm_slot == BLK_VIRT_VMM_SLOT_SECONDARY) {
        return (seL4_CPtr)PD_CNODE_SLOT_GUEST_VMM_SECONDARY_EP;
    }
#endif
    (void)vmm_slot;
    return 0u;
}

/* ── service round ──────────────────────────────────────────────────────── */

static int bv_rescan_needed(void)
{
    for (uint32_t i = 0u; i < AOS_BLK_MAX_CLIENTS; i++) {
        const bv_client_t *c = &g_clients[i];

        if (c->attached &&
            aos_blk_queue_req_length(c->q.req) != 0u &&
            aos_blk_queue_resp_length(c->q.resp) < c->q.capacity) {
            return 1;
        }
    }
    return 0;
}

static void bv_service(void)
{
    for (uint32_t pass = 0u; pass < 4u; pass++) {
        /* Draining: kicks are redundant until we ask for them again. */
        for (uint32_t i = 0u; i < AOS_BLK_MAX_CLIENTS; i++) {
            if (g_clients[i].attached) {
                g_clients[i].q.signal->req_consumer_signalled = 1u;
            }
        }
        bv_fence();

        for (uint32_t i = 0u; i < AOS_BLK_MAX_CLIENTS; i++) {
            bv_client_t *c = &g_clients[i];
            uint32_t n;

            if (!c->attached) {
                continue;
            }
            n = aos_blk_virt_pump(&c->virt);
            if (n > 0u) {
                c->responses += n;
                if (!c->pumped_marked) {
                    c->pumped_marked = 1u;
                    bv_puts("[blk_virt] pumped ");
                    bv_dec(n);
                    bv_puts(" request(s) for client ");
                    bv_dec(c->client_id);
                    bv_puts(c->hw ? " via virtio_blk\n" : " via RAM disk\n");
                }
                bv_notify_vmm(c);
            }
        }

        /* Ask for kicks again, then rescan so nothing enqueued meanwhile
         * waits for the next kick. */
        for (uint32_t i = 0u; i < AOS_BLK_MAX_CLIENTS; i++) {
            if (g_clients[i].attached) {
                g_clients[i].q.signal->req_consumer_signalled = 0u;
            }
        }
        bv_fence();
        if (!bv_rescan_needed()) {
            break;
        }
    }
}

/* ── ATTACH ─────────────────────────────────────────────────────────────── */

static void handle_attach(const sel4_msg_t *req, sel4_msg_t *rep)
{
    uint32_t version = rd32(req->data, 0u);
    uint32_t client_id = rd32(req->data, 4u);
    uint32_t vmm_slot = rd32(req->data, 8u);
    uint32_t media_id = rd32(req->data, 12u);
    uint32_t status = BLK_VIRT_OK;
    uint32_t hw_state = BLK_VIRT_HW_NONE;
    uint64_t capacity = 0u;
    seL4_CPtr vmm_ep = vmm_ep_for_slot(vmm_slot);

    if (version != BLK_VIRT_CONTRACT_VERSION || req->length < 16u) {
        status = BLK_VIRT_ERR_VERSION;
    } else if (client_id >= AOS_BLK_MAX_CLIENTS ||
               media_id >= AOS_HOST_BLK_MEDIA_COUNT || vmm_ep == 0u) {
        status = BLK_VIRT_ERR_BAD_CLIENT;
    } else if (g_clients[client_id].attached) {
        status = BLK_VIRT_ERR_BUSY;
    }

    if (status == BLK_VIRT_OK) {
        bv_client_t *c = &g_clients[client_id];
        uint64_t host_sectors = 0u;
        bool host_read_only = true;
        uint32_t rc;

        c->client_id = client_id;
        c->media_id = media_id;
        c->vmm_ep = vmm_ep;
        c->hw = 0u;
        /* The VMM zeroed its queues before attaching; only bind. */
        aos_blk_client_bind((uint8_t *)AOS_BLK_SHMEM_VA, client_id, &c->q);
        aos_blk_virt_reset(&c->virt);

        rc = host_blk_call(media_id, AOS_HOST_BLK_OP_INFO, 0u, 0u,
                           &host_sectors, &host_read_only);
        if (rc == AOS_HOST_BLK_OK &&
            host_sectors >= (AOS_BLK_TRANSFER_SIZE / AOS_HOST_BLK_SECTOR_SIZE)) {
            uint64_t host_blocks = host_sectors /
                (AOS_BLK_TRANSFER_SIZE / AOS_HOST_BLK_SECTOR_SIZE);

            if (host_blocks > UINT32_MAX) {
                host_blocks = UINT32_MAX;
            }
            aos_blk_storage_init(c->q.info, (uint32_t)host_blocks);
            /* Media write policy comes from the driver (VIRTIO_BLK_F_RO). */
            c->q.info->read_only = host_read_only;
            /*
             * ISO9660 requires a logical sector no larger than 2048 bytes.
             * Requests still batch through 4 KiB sDDF transfer units, but
             * the guest-visible VirtIO geometry is 512-byte sectors.
             */
            c->q.info->sector_size = AOS_HOST_BLK_SECTOR_SIZE;
            c->q.info->block_size = 0u;
            aos_blk_virt_set_backend(&c->virt, host_blk_backend, c);
            c->hw = 1u;
            hw_state = BLK_VIRT_HW_VIRTIO_BLK;
            capacity = host_blocks;
        } else {
            aos_blk_storage_init(c->q.info, AOS_BLK_DISK_BLOCKS);
            aos_blk_virt_set_disk(&c->virt, g_ram_disk[client_id],
                                  AOS_BLK_DISK_BLOCKS);
            capacity = AOS_BLK_DISK_BLOCKS;
        }
        (void)aos_blk_virt_add_client(&c->virt, &c->q);
        /* Protocol state: want kicks. */
        c->q.signal->req_consumer_signalled = 0u;
        bv_fence();
        c->attached = 1u;

        bv_puts("[blk_virt] ATTACH client=");
        bv_dec(client_id);
        bv_puts(" vmm_slot=");
        bv_dec(vmm_slot);
        bv_puts(" media=");
        bv_dec(media_id);
        bv_puts(c->hw ? " hw=1\n" : " hw=0 (RAM disk backend)\n");
        if (c->hw) {
            bv_puts("[blk_virt] host media ");
            bv_dec(media_id);
            bv_puts(" ready sectors=");
            bv_dec(host_sectors);
            bv_puts(host_read_only ? " read-only" : " writable");
            bv_puts("\n");
        } else {
            bv_puts("[blk_virt] host unavailable rc=");
            bv_dec(rc);
            bv_puts("; serving ");
            bv_dec(AOS_BLK_DISK_BLOCKS);
            bv_puts("-block RAM disk\n");
        }
    } else {
        bv_puts("[blk_virt] ATTACH rejected status=");
        bv_dec(status);
        bv_puts("\n");
    }

    wr32(rep->data, 0u, status);
    wr32(rep->data, 4u, BLK_VIRT_CONTRACT_VERSION);
    wr32(rep->data, 8u, hw_state);
    wr32(rep->data, 12u, 0u);
    wr32(rep->data, 16u, (uint32_t)capacity);
    wr32(rep->data, 20u, (uint32_t)(capacity >> 32));
    rep->length = (uint32_t)sizeof(blk_virt_attach_reply_t);
    rep->opcode = SEL4_ERR_OK;
}

/* ── main loop ──────────────────────────────────────────────────────────── */

static void blk_virt_run(seL4_CPtr ep)
{
    for (;;) {
        seL4_Word badge = 0u;
        sel4_msg_t req = {0};
        sel4_msg_t rep = {0};
#ifdef CONFIG_KERNEL_MCS
        seL4_MessageInfo_t info = seL4_Recv(ep, &badge, AGENTOS_IPC_REPLY_CAP);
#else
        seL4_MessageInfo_t info = seL4_Recv(ep, &badge);
#endif
        seL4_Word label = seL4_MessageInfo_get_label(info);
        (void)badge;

        if (label == BLK_VIRT_OP_ATTACH) {
            _sel4_mrs_to_msg(&req);
            handle_attach(&req, &rep);
            _sel4_msg_to_mrs(&rep);
            seL4_MessageInfo_t reply = seL4_MessageInfo_new(
                (seL4_Word)rep.opcode, 0u, 0u, (seL4_Word)_SEL4_MR_COUNT);
#ifdef CONFIG_KERNEL_MCS
            seL4_Send(AGENTOS_IPC_REPLY_CAP, reply);
#else
            seL4_Reply(reply);
#endif
            /* A client may have queued requests before its ATTACH reply
             * landed; serve them without waiting for a kick. */
            bv_service();
            continue;
        }
        if (label == BLK_VIRT_EVENT_KICK) {
            bv_service();
            continue;
        }
        /* Anything else (stray fault labels, unknown events) is ignored. */
    }
}

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    agentos_log_boot("blk_virt");
    register_with_nameserver(ns_ep);
    bv_puts("[blk_virt] READY: contract v1, shared block region + driver DMA window mapped, no device caps\n");
    blk_virt_run(my_ep);
}
