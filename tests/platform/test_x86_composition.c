#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "system_desc.h"
#include "contracts/guest_ram_caps.h"

extern const system_desc_t system_desc_x86_64;

static unsigned position(unsigned service)
{
    for (unsigned i = 0; i < system_desc_x86_64.pd_count; ++i)
        if (system_desc_x86_64.pds[i].self_svc_id == service) return i;
    return SYSTEM_MAX_PDS;
}

int main(void)
{
    const system_desc_t *s = &system_desc_x86_64;
#ifdef AGENTOS_X86_DUAL_GUEST
    const unsigned guests = 2;
    assert(s->pd_count == 13);
#else
    const unsigned guests = 1;
    assert(s->pd_count == 10);
    assert(position(SVC_ID_GUEST_VMM_SECONDARY) == SYSTEM_MAX_PDS);
    assert(position(SVC_ID_X86_SECONDARY_RUNNER) == SYSTEM_MAX_PDS);
#endif
    for (unsigned i = 0; i < s->pd_count; ++i) {
        assert(s->pds[i].name[0] && s->pds[i].elf_path[0]);
        for (unsigned j = i + 1; j < s->pd_count; ++j) {
            assert(s->pds[i].self_svc_id != s->pds[j].self_svc_id);
            assert(strcmp(s->pds[i].name, s->pds[j].name));
        }
    }
    unsigned coordinators[] = {SVC_ID_GUEST_VMM_PRIMARY, SVC_ID_GUEST_VMM_SECONDARY};
    unsigned runners[][2] = {{SVC_ID_X86_RUNNER, SVC_ID_X86_AP_RUNNER},
        {SVC_ID_X86_SECONDARY_RUNNER, SVC_ID_X86_SECONDARY_AP_RUNNER}};
    unsigned manager_position = position(SVC_ID_VM_MANAGER);
    assert(manager_position < s->pd_count);
    const pd_desc_t *manager = &s->pds[manager_position];
    assert(manager->init_ep_count == guests);
    for (unsigned owner = 0; owner < guests; ++owner) {
        unsigned vmm = position(coordinators[owner]);
        assert(vmm < s->pd_count);
        const pd_desc_t *pd = &s->pds[vmm];
        assert(pd->cnode_size_bits == AOS_GUEST_RAM_CNODE_BITS);
        assert(pd->device_frame_count == 0 && pd->irq_count == 0);
        assert(pd->init_ep_count == 3);
        assert(position(runners[owner][0]) < vmm);
        assert(position(runners[owner][1]) < vmm);
        /* Each guest has a public control route, with distinct cap slots. */
        assert(manager->init_eps[owner].service_id == coordinators[owner]);
        if (owner) assert(manager->init_eps[0].cnode_slot != manager->init_eps[1].cnode_slot);
    }
    puts("PASS: x86 composition has unique identities, private runner pairs and lifecycle routes");
    return 0;
}
