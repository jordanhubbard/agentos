#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <platform/x86_runner_ownership.h>

int main(void)
{
    aos_x86_runner_owner_t owners[] = {
        {.coordinator = 16, .service = {36, 37}},
        {.coordinator = 17, .service = {38, 39}},
    };
    assert(aos_x86_runner_owner(owners, 2, 99) == NULL);
    assert(aos_x86_runner_register(owners, 2, 39, 104, 4));
    assert(aos_x86_runner_register(owners, 2, 36, 101, 1));
    aos_x86_runner_owner_t saved[2];
    memcpy(saved, owners, sizeof(saved));
    assert(!aos_x86_runner_register(owners, 2, 36, 105, 5));
    assert(!aos_x86_runner_register(owners, 2, 37, 104, 5));
    assert(!aos_x86_runner_register(owners, 2, 37, 105, 4));
    assert(!aos_x86_runner_register(owners, 2, 37, 0, 5));
    assert(!aos_x86_runner_register(owners, 2, 99, 105, 5));
    assert(memcmp(saved, owners, sizeof(saved)) == 0);
    assert(aos_x86_runner_register(owners, 2, 38, 103, 3));
    assert(aos_x86_runner_register(owners, 2, 37, 102, 2));
    aos_x86_runner_owner_t *primary = aos_x86_runner_owner(owners, 2, 16);
    aos_x86_runner_owner_t *secondary = aos_x86_runner_owner(owners, 2, 17);
    assert(primary && secondary && primary != secondary);
    assert(primary->tcb[0] == 101 && primary->tcb[1] == 102);
    assert(primary->pd_index[0] == 1 && primary->pd_index[1] == 2);
    assert(secondary->tcb[0] == 103 && secondary->tcb[1] == 104);
    assert(secondary->pd_index[0] == 3 && secondary->pd_index[1] == 4);
    /* Ambiguous static topology must not partially bind an executor. */
    owners[1].service[0] = owners[0].service[0];
    memset(owners[0].tcb, 0, sizeof(owners[0].tcb));
    memset(owners[1].tcb, 0, sizeof(owners[1].tcb));
    memcpy(saved, owners, sizeof(saved));
    assert(!aos_x86_runner_register(owners, 2, 36, 101, 1));
    assert(memcmp(saved, owners, sizeof(saved)) == 0);
    puts("PASS: separate runner owners, out-of-order registration, duplicate rejection without mutation");
    return 0;
}
