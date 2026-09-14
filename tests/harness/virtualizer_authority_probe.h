/* Compiled only into the explicit authority-probe VMM image. */
#include <contracts/net_virt_contract.h>
#include <contracts/blk_virt_contract.h>
#include "serial_log.h"

static bool authority_probe_call(seL4_CPtr ep, uint32_t opcode,
                                 uint32_t version, uint32_t client,
                                 uint32_t slot, uint32_t media,
                                 uint32_t expected)
{
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = opcode;
    req.length = opcode == BLK_VIRT_OP_ATTACH ? 16u : 12u;
    serial_log_wr32(req.data, 0u, version);
    serial_log_wr32(req.data, 4u, client);
    serial_log_wr32(req.data, 8u, slot);
    serial_log_wr32(req.data, 12u, media);
    sel4_call(ep, &req, &rep);
    return rep.opcode == SEL4_ERR_OK && rep.length >= 4u &&
           serial_log_rd32(rep.data, 0u) == expected;
}

static void virtualizer_authority_probe(void)
{
    const uint32_t own = AGENTOS_VIRT_AUTHORITY_PROBE - 1u;
    bool good = true;
    const uint32_t ids[] = {0u, 1u, UINT32_MAX};
    for (unsigned c = 0; c < 3; ++c) {
        for (unsigned s = 0; s < 3; ++s) {
            if (ids[c] != own || ids[s] != own) {
                good &= authority_probe_call(PD_CNODE_SLOT_NET_VIRT_EP,
                    NET_VIRT_OP_ATTACH, NET_VIRT_CONTRACT_VERSION,
                    ids[c], ids[s], 0u, NET_VIRT_ERR_BAD_CLIENT);
            }
            for (unsigned m = 0; m < 3; ++m) {
                if (ids[c] == own && ids[s] == own && ids[m] == own) continue;
                good &= authority_probe_call(PD_CNODE_SLOT_BLK_VIRT_EP,
                    BLK_VIRT_OP_ATTACH, BLK_VIRT_CONTRACT_VERSION,
                    ids[c], ids[s], ids[m], BLK_VIRT_ERR_BAD_CLIENT);
            }
        }
    }
    /* Rejections must not consume our legitimate attachment. */
    good &= authority_probe_call(PD_CNODE_SLOT_NET_VIRT_EP,
        NET_VIRT_OP_ATTACH, NET_VIRT_CONTRACT_VERSION, own, own, 0u, NET_VIRT_OK);
    good &= authority_probe_call(PD_CNODE_SLOT_BLK_VIRT_EP,
        BLK_VIRT_OP_ATTACH, BLK_VIRT_CONTRACT_VERSION, own, own, own, BLK_VIRT_OK);
    serial_log_t log = {.ep = PD_CNODE_SLOT_SERIAL_EP};
    serial_log_puts(&log, good ?
        "[authority-test] spoofed attachments rejected; assigned net/block clients accepted\n" :
        "[authority-test] FAIL: attachment authority mismatch\n");
    for (;;) seL4_Yield();
}
