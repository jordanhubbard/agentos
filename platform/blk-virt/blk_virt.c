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
 * This is the scaffold step: the PD boots, registers with the nameserver,
 * and answers ATTACH with BLK_VIRT_ERR_UNAVAILABLE.  Request movement is
 * still done by the pump linked into the VMM (platform/blk-virt/
 * vmm_virtio_blk.c) until the next step moves it here.
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

/* Unmapped: log_drain_write falls back to the (release-silent) debug putc.
 * Visible diagnostics go through serial_pd via serial_log_t. */
uintptr_t log_drain_rings_vaddr;

static serial_log_t g_log = { .ep = PD_CNODE_SLOT_SERIAL_EP };

static void bv_puts(const char *s)
{
    serial_log_puts(&g_log, s);
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

static void handle_attach(const sel4_msg_t *req, sel4_msg_t *rep)
{
    uint32_t version = rd32(req->data, 0u);
    uint32_t client_id = rd32(req->data, 4u);
    uint32_t media_id = rd32(req->data, 12u);
    uint32_t status = BLK_VIRT_ERR_UNAVAILABLE;

    if (version != BLK_VIRT_CONTRACT_VERSION || req->length < 16u) {
        status = BLK_VIRT_ERR_VERSION;
    } else if (client_id >= AOS_BLK_MAX_CLIENTS ||
               media_id >= AOS_HOST_BLK_MEDIA_COUNT) {
        status = BLK_VIRT_ERR_BAD_CLIENT;
    }
    wr32(rep->data, 0u, status);
    wr32(rep->data, 4u, BLK_VIRT_CONTRACT_VERSION);
    wr32(rep->data, 8u, BLK_VIRT_HW_NONE);
    wr32(rep->data, 12u, 0u);
    wr32(rep->data, 16u, 0u);
    wr32(rep->data, 20u, 0u);
    rep->length = (uint32_t)sizeof(blk_virt_attach_reply_t);
    rep->opcode = SEL4_ERR_OK;
}

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
            continue;
        }
        /* BLK_VIRT_EVENT_KICK: nothing to move yet. */
    }
}

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    agentos_log_boot("blk_virt");
    register_with_nameserver(ns_ep);
    bv_puts("[blk_virt] READY: scaffold, contract v1, shared block region mapped\n");
    blk_virt_run(my_ep);
}
