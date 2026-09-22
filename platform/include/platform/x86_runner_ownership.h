#ifndef AOS_X86_RUNNER_OWNERSHIP_H
#define AOS_X86_RUNNER_OWNERSHIP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Root-owned provisioning state, never supplied by a guest or coordinator. */
typedef struct {
    uint32_t coordinator;
    uint32_t service[2];
    uintptr_t tcb[2];
    uint32_t pd_index[2];
} aos_x86_runner_owner_t;

static inline aos_x86_runner_owner_t *aos_x86_runner_owner(
    aos_x86_runner_owner_t *owners, size_t count, uint32_t coordinator)
{
    for (size_t i = 0; i < count; i++)
        if (owners[i].coordinator == coordinator) return &owners[i];
    return NULL;
}

static inline bool aos_x86_runner_register(aos_x86_runner_owner_t *owners,
    size_t count, uint32_t service, uintptr_t tcb, uint32_t pd_index)
{
    if (!tcb) return false;
    aos_x86_runner_owner_t *owner = NULL;
    unsigned cpu = 0;
    for (size_t i = 0; i < count; i++) {
        for (unsigned n = 0; n < 2; n++) {
            if (owners[i].tcb[n] == tcb) return false;
            if (owners[i].tcb[n] && owners[i].pd_index[n] == pd_index) return false;
            if (owners[i].service[n] == service) {
                if (owner || owners[i].tcb[n]) return false;
                owner = &owners[i];
                cpu = n;
            }
        }
    }
    if (!owner) return false;
    owner->tcb[cpu] = tcb;
    owner->pd_index[cpu] = pd_index;
    return true;
}
#endif
