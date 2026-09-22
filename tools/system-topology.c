/* SPDX-License-Identifier: BSD-2-Clause
 * Render initial endpoint grants directly from the compiled shipping table.
 * This reports descriptor authority, not dynamic mappings or runtime traffic.
 */
#include <stdio.h>
#include <string.h>
#include "system_desc.h"

extern const system_desc_t system_desc_aarch64;

static int selected(const char *name, int argc, char **argv)
{
    if (argc == 1) return 1;
    for (int i = 1; i < argc; ++i)
        if (strcmp(name, argv[i]) == 0) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    const system_desc_t *system = &system_desc_aarch64;
    if (system->pd_count > SYSTEM_MAX_PDS) return 1;
    for (int a = 1; a < argc; ++a) {
        int found = 0;
        for (unsigned i = 0; i < system->pd_count; ++i)
            found |= strcmp(argv[a], system->pds[i].name) == 0;
        if (!found) {
            fprintf(stderr, "Unknown protection domain: %s\n", argv[a]);
            return 1;
        }
    }
    puts("flowchart LR");
    for (unsigned i = 0; i < system->pd_count; ++i) {
        const pd_desc_t *caller = &system->pds[i];
        if (caller->init_ep_count > PD_MAX_INIT_EPS) return 1;
        if (!selected(caller->name, argc, argv)) continue;
        printf("  %s[%s]\n", caller->name, caller->name);
        for (unsigned e = 0; e < caller->init_ep_count; ++e) {
            const pd_init_ep_t *grant = &caller->init_eps[e];
            for (unsigned j = 0; j < system->pd_count; ++j) {
                const pd_desc_t *server = &system->pds[j];
                if (grant->service_id && server->self_svc_id == grant->service_id &&
                    selected(server->name, argc, argv))
                    printf("  %s -->|slot %u| %s\n", caller->name,
                           grant->cnode_slot, server->name);
            }
        }
    }
    return ferror(stdout) ? 1 : 0;
}
