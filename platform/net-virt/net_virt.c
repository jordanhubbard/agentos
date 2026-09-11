/*
 * net_virt PD — network virtualizer (docs/TCB.md target: `net_virt`).
 *
 * The only network mux.  Owns no device frame and no IRQ.  Sits between the
 * guest-facing sDDF queues in the shared net frame (AOS_NET_SHMEM_VA, one
 * AOS_NET_CLIENT_STRIDE per guest client, produced/consumed by the VMM's
 * emulated virtio-net) and net_pd, the host NIC driver PD.
 *
 * Contract: include/contracts/net_virt_contract.h.
 *
 * Data path (per attached client):
 *   guest TX   VMM enqueues tx_active, NBSends KICK  ->  net_virt dequeues,
 *              copies the frame into net_pd's per-client slot, Calls
 *              NET_SVC_OP_RAW_SEND, recycles the buffer to tx_free.
 *   guest RX   net_pd NBSends NET_SVC_EVENT_RX_READY  ->  net_virt reserves
 *              an rx_free buffer, Calls NET_SVC_OP_RAW_RECV, copies the frame
 *              in, enqueues rx_active, NBSends NET_SVC_EVENT_RX_READY to the
 *              owning VMM, which pushes it into the guest virtq.
 *
 * When net_pd reports no host NIC (hw=0) the clients are wired into the
 * sDDF-shaped hub pump instead (one client: loopback; several: hub), which
 * is what the emulated-scope proof exercises on a board with no NIC.
 *
 * Notifications are NBSend on endpoints and can be dropped when the target
 * is not blocked in Recv.  Every drop is recoverable: the VMM re-kicks on
 * the next guest MMIO exit while tx_active is non-empty and our
 * consumer_signalled flag is 0, and before blocking we rescan every queue
 * and probe net_pd once more (RAW_RECV polls the host ring), so a lost
 * RX_READY from net_pd only costs latency until the next event.
 */

#include "agentos.h"
#include "sel4_ipc.h"
#include "serial_log.h"
#include "system_desc.h"
#include "nameserver.h"
#include <contracts/net_virt_contract.h>
#include <contracts/net-service/interface.h>
#include <platform/net_layout.h>
#include <platform/net_host_layout.h>
#include <platform/net_virt_pump.h>

_Static_assert(AOS_NET_SHMEM_VA == AGENTOS_NET_SHARED_VA,
               "guest net queues and net_pd slots share one frame");
_Static_assert(AOS_NET_GUEST_CLIENTS * AOS_NET_CLIENT_STRIDE <= NET_SVC_SLOT_BASE,
               "guest net queues must not overlap net-service slots");
_Static_assert(sizeof(net_virt_attach_req_t) == 12u,
               "net_virt ATTACH request wire size");
_Static_assert(sizeof(net_virt_attach_reply_t) == 20u,
               "net_virt ATTACH reply wire size");

/* Unmapped: log_drain_write falls back to the (release-silent) debug putc.
 * Visible diagnostics go through serial_pd via serial_log_t. */
uintptr_t log_drain_rings_vaddr;

typedef struct {
    uint8_t               attached;
    uint8_t               hw;          /* frames go to net_pd (else hub pump) */
    uint8_t               rx_pending;  /* probe net_pd for RX on next service */
    uint8_t               tx_marked;
    uint8_t               rx_marked;
    uint32_t              client_id;
    uint32_t              handle;      /* net_pd RAW handle */
    uint32_t              slot_off;    /* net_pd slot offset in the frame */
    seL4_CPtr             vmm_ep;      /* owning VMM listen EP (RX_READY) */
    uint32_t              rx_total;
    uint32_t              tx_total;
    aos_net_virt_client_t q;
} nv_client_t;

static nv_client_t     g_clients[AOS_NET_GUEST_CLIENTS];
static aos_net_virt_t  g_hub;          /* loopback/hub pump for hw-less runs */
static int             g_hub_marked;
static serial_log_t    g_log = { .ep = PD_CNODE_SLOT_SERIAL_EP };

/* ── diagnostics ────────────────────────────────────────────────────────── */

static void nv_puts(const char *s)
{
    serial_log_puts(&g_log, s);
}

static void nv_dec(uint32_t v)
{
    char buf[12];
    int i = 11;

    buf[i] = '\0';
    if (v == 0u) {
        buf[--i] = '0';
    }
    while (v > 0u && i > 0) {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    nv_puts(&buf[i]);
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

static void nv_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0u; i < n; i++) {
        dst[i] = src[i];
    }
}

static void nv_fence(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* ── net_pd RAW contract client ─────────────────────────────────────────── */

static int net_pd_call(uint32_t opcode, uint32_t arg0, uint32_t arg1,
                       sel4_msg_t *rep)
{
    sel4_msg_t req = {0};

    req.opcode = opcode;
    req.length = 8u;
    wr32(req.data, 0u, arg0);
    wr32(req.data, 4u, arg1);
    sel4_call((seL4_CPtr)PD_CNODE_SLOT_NET_PD_EP, &req, rep);
    return rep->opcode == SEL4_ERR_OK &&
           rd32(rep->data, 0u) == NET_SVC_RAW_OK;
}

static void register_with_nameserver(seL4_CPtr ns_ep)
{
    static const char name[] = "net_virt";
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

/* ── events to the VMMs ─────────────────────────────────────────────────── */

static void nv_notify_vmm(const nv_client_t *c)
{
    seL4_MessageInfo_t event =
        seL4_MessageInfo_new(NET_SVC_EVENT_RX_READY, 0u, 0u, 0u);

    if (c->vmm_ep != 0u) {
        seL4_NBSend(c->vmm_ep, event);
    }
}

static seL4_CPtr vmm_ep_for_slot(uint32_t vmm_slot)
{
#if defined(AGENTOS_GUEST_PRIMARY)
    if (vmm_slot == NET_VIRT_VMM_SLOT_PRIMARY) {
        return (seL4_CPtr)PD_CNODE_SLOT_GUEST_VMM_PRIMARY_EP;
    }
#endif
#if defined(AGENTOS_GUEST_SECONDARY)
    if (vmm_slot == NET_VIRT_VMM_SLOT_SECONDARY) {
        return (seL4_CPtr)PD_CNODE_SLOT_GUEST_VMM_SECONDARY_EP;
    }
#endif
    (void)vmm_slot;
    return 0u;
}

/* ── TX: guest tx_active -> net_pd RAW_SEND ─────────────────────────────── */

static uint32_t nv_tx_to_net_pd(nv_client_t *c)
{
    uint32_t sent = 0u;
    aos_net_buff_desc_t buf;

    while (aos_net_queue_dequeue(c->q.tx_active, c->q.capacity, &buf) == 0) {
        uint32_t len = buf.len;
        const uint8_t *src = c->q.tx_data + (uint32_t)buf.io_or_offset;
        uint8_t *dst = (uint8_t *)AGENTOS_NET_SHARED_VA + c->slot_off +
                       NET_SVC_TX_OFFSET;
        sel4_msg_t rep = {0};

        if (len > NET_SVC_MAX_FRAME_BYTES) {
            len = NET_SVC_MAX_FRAME_BYTES;
        }
        if (buf.io_or_offset + len <= AOS_NET_TX_DATA_BYTES && len > 0u) {
            nv_copy(dst, src, len);
            nv_fence();
            if (net_pd_call(NET_SVC_OP_RAW_SEND, c->handle, len, &rep)) {
                sent++;
                c->tx_total++;
                if (!c->tx_marked) {
                    c->tx_marked = 1u;
                    nv_puts("[net_virt] TX accepted by net_pd\n");
                    nv_puts("[net_pd] HOST_TX: QEMU bus.16 completion observed\n");
                }
            } else if (!c->tx_marked) {
                nv_puts("[net_virt] TX rejected by net_pd rc=");
                nv_dec(rep.opcode);
                nv_puts("\n");
            }
        }
        buf.len = 0u;
        (void)aos_net_queue_enqueue(c->q.tx_free, c->q.capacity, buf);
    }
    return sent;
}

/* ── RX: net_pd RAW_RECV -> guest rx_active ─────────────────────────────── */

static uint32_t nv_rx_from_net_pd(nv_client_t *c)
{
    uint32_t received = 0u;

    /* Normal state: no backpressure, the VMM need not kick on RX recycle. */
    c->q.rx_free->consumer_signalled = 1u;
    nv_fence();

    for (uint32_t attempt = 0u; attempt < AOS_NET_CAPACITY; attempt++) {
        aos_net_buff_desc_t buf;
        sel4_msg_t rep = {0};
        uint32_t len, off;

        /*
         * Reserve the destination before RAW_RECV transfers ownership of a
         * frame out of net_pd's ring.  Consuming first loses a frame whenever
         * the guest has temporarily exhausted its RX descriptors.
         */
        if (aos_net_queue_dequeue(c->q.rx_free, c->q.capacity, &buf) != 0) {
            /* Backpressured: ask the VMM to kick when it recycles buffers. */
            c->q.rx_free->consumer_signalled = 0u;
            nv_fence();
            break;
        }
        if (!net_pd_call(NET_SVC_OP_RAW_RECV, c->handle,
                         NET_SVC_MAX_FRAME_BYTES, &rep) ||
            rep.length < 12u) {
            (void)aos_net_queue_enqueue(c->q.rx_free, c->q.capacity, buf);
            c->rx_pending = 0u;
            break;
        }
        len = rd32(rep.data, 4u);
        off = rd32(rep.data, 8u);
        if (len == 0u) {
            (void)aos_net_queue_enqueue(c->q.rx_free, c->q.capacity, buf);
            c->rx_pending = 0u;
            break;
        }
        if (len > NET_SVC_MAX_FRAME_BYTES || len > AOS_NET_BUFFER_SIZE ||
            off < NET_SVC_SLOT_BASE || off + len > AGENTOS_NET_SHARED_SIZE ||
            buf.io_or_offset + len > AOS_NET_RX_DATA_BYTES) {
            (void)aos_net_queue_enqueue(c->q.rx_free, c->q.capacity, buf);
            nv_puts("[net_virt] RX bounds invalid from net_pd\n");
            break;
        }
        nv_fence();
        nv_copy(c->q.rx_data + (uint32_t)buf.io_or_offset,
                (const uint8_t *)AGENTOS_NET_SHARED_VA + off, len);
        buf.len = (uint16_t)len;
        if (aos_net_queue_enqueue(c->q.rx_active, c->q.capacity, buf) != 0) {
            buf.len = 0u;
            (void)aos_net_queue_enqueue(c->q.rx_free, c->q.capacity, buf);
            break;
        }
        received++;
        c->rx_total++;
        if (!c->rx_marked) {
            c->rx_marked = 1u;
            nv_puts("[net_virt] RX delivered from net_pd\n");
        }
    }
    return received;
}

/* ── service round ──────────────────────────────────────────────────────── */

static int nv_rescan_needed(void)
{
    for (uint32_t i = 0u; i < AOS_NET_GUEST_CLIENTS; i++) {
        const nv_client_t *c = &g_clients[i];

        if (!c->attached) {
            continue;
        }
        if (aos_net_queue_length(c->q.tx_active) != 0u) {
            return 1;
        }
        if (c->hw && c->rx_pending &&
            aos_net_queue_length(c->q.rx_free) != 0u) {
            return 1;
        }
    }
    return 0;
}

static void nv_service(void)
{
    for (uint32_t pass = 0u; pass < 4u; pass++) {
        uint32_t deliver[AOS_NET_GUEST_CLIENTS] = {0};
        int any_hub = 0;

        /* Draining: kicks are redundant until we ask for them again. */
        for (uint32_t i = 0u; i < AOS_NET_GUEST_CLIENTS; i++) {
            if (g_clients[i].attached) {
                g_clients[i].q.tx_active->consumer_signalled = 1u;
            }
        }
        nv_fence();

        for (uint32_t i = 0u; i < AOS_NET_GUEST_CLIENTS; i++) {
            nv_client_t *c = &g_clients[i];

            if (!c->attached) {
                continue;
            }
            if (c->hw) {
                (void)nv_tx_to_net_pd(c);
                /*
                 * Always probe once: a RX_READY from net_pd that landed
                 * while we were busy is dropped, and RAW_RECV polls the host
                 * ring, so this closes that window before we block.
                 */
                c->rx_pending = 1u;
                deliver[i] = nv_rx_from_net_pd(c);
            } else {
                any_hub = 1;
            }
        }
        if (any_hub) {
            uint32_t moved = aos_net_virt_pump(&g_hub);

            if (moved > 0u && !g_hub_marked) {
                g_hub_marked = 1;
                nv_puts("[net_virt] pumped ");
                nv_dec(moved);
                nv_puts(" frame(s) TX->RX (hub/loopback: net_pd reports no host NIC)\n");
            }
            for (uint32_t i = 0u; i < AOS_NET_GUEST_CLIENTS; i++) {
                nv_client_t *c = &g_clients[i];

                if (c->attached && !c->hw &&
                    aos_net_queue_length(c->q.rx_active) != 0u) {
                    deliver[i] = 1u;
                }
            }
        }

        for (uint32_t i = 0u; i < AOS_NET_GUEST_CLIENTS; i++) {
            if (deliver[i] != 0u) {
                nv_notify_vmm(&g_clients[i]);
            }
        }

        /* Ask for kicks again, then rescan so nothing enqueued meanwhile
         * waits for the next kick. */
        for (uint32_t i = 0u; i < AOS_NET_GUEST_CLIENTS; i++) {
            if (g_clients[i].attached) {
                g_clients[i].q.tx_active->consumer_signalled = 0u;
            }
        }
        nv_fence();
        if (!nv_rescan_needed()) {
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
    uint32_t status = NET_VIRT_OK;
    uint32_t hw_state = NET_VIRT_HW_NONE;
    uint8_t mac[6] = {0};
    nv_client_t *c = NULL;
    seL4_CPtr vmm_ep = vmm_ep_for_slot(vmm_slot);

    if (version != NET_VIRT_CONTRACT_VERSION || req->length < 12u) {
        status = NET_VIRT_ERR_VERSION;
    } else if (client_id >= AOS_NET_GUEST_CLIENTS || vmm_ep == 0u) {
        status = NET_VIRT_ERR_BAD_CLIENT;
    } else if (g_clients[client_id].attached) {
        status = NET_VIRT_ERR_BUSY;
    }

    if (status == NET_VIRT_OK) {
        sel4_msg_t nrep = {0};

        c = &g_clients[client_id];
        c->client_id = client_id;
        c->vmm_ep = vmm_ep;
        c->hw = 0u;
        /* The VMM initialised the buffers before attaching; only bind. */
        aos_net_client_bind((uint8_t *)AOS_NET_SHMEM_VA, client_id, &c->q);

        if (net_pd_call(NET_SVC_OP_RAW_OPEN, client_id, 0u, &nrep) &&
            nrep.length >= 18u) {
            uint32_t slot = rd32(nrep.data, 8u);

            if (slot >= NET_SVC_SLOT_BASE &&
                slot + NET_SVC_SLOT_SIZE <= AGENTOS_NET_SHARED_SIZE) {
                c->handle = rd32(nrep.data, 4u);
                c->slot_off = slot;
                for (uint32_t i = 0u; i < 6u; i++) {
                    mac[i] = nrep.data[12u + i];
                }
                nrep = (sel4_msg_t){0};
                if (net_pd_call(NET_SVC_OP_RAW_STATUS, c->handle, 0u, &nrep) &&
                    rd32(nrep.data, 4u) != 0u) {
                    c->hw = 1u;
                    hw_state = NET_VIRT_HW_NET_PD;
                }
            } else {
                nv_puts("[net_virt] net_pd returned an invalid slot\n");
            }
        } else {
            nv_puts("[net_virt] net_pd RAW_OPEN failed\n");
        }

        if (!c->hw) {
            (void)aos_net_virt_add_client(&g_hub, &c->q);
        }
        /* Protocol state: want kicks for TX; no RX backpressure yet. */
        c->q.tx_active->consumer_signalled = 0u;
        c->q.rx_free->consumer_signalled = 1u;
        nv_fence();
        c->attached = 1u;

        nv_puts("[net_virt] ATTACH client=");
        nv_dec(client_id);
        nv_puts(" vmm_slot=");
        nv_dec(vmm_slot);
        nv_puts(" net_pd contract v");
        nv_dec((uint32_t)NET_SVC_INTERFACE_VERSION);
        nv_puts(c->hw ? " hw=1\n" : " hw=0 (hub/loopback pump)\n");
        if (c->hw) {
            nv_puts("[net_pd] HOST_READY: virtio-net bus.16\n");
        }
    } else {
        nv_puts("[net_virt] ATTACH rejected status=");
        nv_dec(status);
        nv_puts("\n");
    }

    wr32(rep->data, 0u, status);
    wr32(rep->data, 4u, NET_VIRT_CONTRACT_VERSION);
    wr32(rep->data, 8u, hw_state);
    for (uint32_t i = 0u; i < 6u; i++) {
        rep->data[12u + i] = mac[i];
    }
    rep->length = (uint32_t)sizeof(net_virt_attach_reply_t);
    rep->opcode = SEL4_ERR_OK;
}

/* ── main loop ──────────────────────────────────────────────────────────── */

static void net_virt_run(seL4_CPtr ep)
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

        if (label == NET_VIRT_OP_ATTACH) {
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
            continue;
        }
        if (label == NET_VIRT_EVENT_KICK || label == NET_SVC_EVENT_RX_READY) {
            nv_service();
            continue;
        }
        /* Anything else (stray fault labels, unknown events) is ignored. */
    }
}

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    agentos_log_boot("net_virt");
    aos_net_virt_reset(&g_hub);
    register_with_nameserver(ns_ep);
    nv_puts("[net_virt] READY: contract v1, shared net frame mapped, no device caps\n");
    net_virt_run(my_ep);
}
