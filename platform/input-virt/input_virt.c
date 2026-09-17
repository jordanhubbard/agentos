/* SPDX-License-Identifier: BSD-2-Clause */
#include <platform/input.h>
#include "system_desc.h"
#include <sel4/sel4.h>

void pd_main(seL4_CPtr endpoint, seL4_CPtr nameserver)
{
    (void)endpoint; (void)nameserver;
    aos_input_service_t service;
    aos_input_client_region_t *clients[AOS_INPUT_CLIENTS];
    for (unsigned i=0;i<AOS_INPUT_CLIENTS;++i)
        clients[i]=(void *)(AOS_INPUT_SHMEM_VA+i*AOS_INPUT_FRAME_SIZE);
    if (aos_input_service_init(&service,(void *)AOS_INPUT_FRONTEND_VA,clients,3)!=0)
        for (;;) { seL4_Word badge; seL4_Wait(PD_CNODE_SLOT_INPUT_WAIT,&badge); }
    for (;;) {
        uint32_t ready;
        unsigned progress=aos_input_pump(&service,&ready);
        for (unsigned i=0;i<AOS_INPUT_CLIENTS;++i)
            if (ready & (1u<<i)) seL4_Signal(PD_CNODE_SLOT_INPUT_PEER_NOTIFY+i);
        if (progress) seL4_Signal(PD_CNODE_SLOT_INPUT_PEER_NOTIFY+AOS_INPUT_CLIENTS);
        else { seL4_Word badge; seL4_Wait(PD_CNODE_SLOT_INPUT_WAIT,&badge); }
    }
}
