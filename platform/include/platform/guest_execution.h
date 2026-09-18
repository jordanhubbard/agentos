#ifndef AOS_PLATFORM_GUEST_EXECUTION_H
#define AOS_PLATFORM_GUEST_EXECUTION_H
#include <stdbool.h>

/* After terminal teardown, rebuild stopped execution objects in a fresh
 * guest VSpace. The execution pool and exchange TCB/SC/VSpace slots must be
 * empty; the exchange's existing VMM fault endpoint stays intact.
 * On failure, release execution and paging before retrying. Success only
 * publishes capabilities: vm_manager must configure scheduling before BOOT. */
bool aos_vmm_guest_execution_rebuild(void);
bool aos_vmm_guest_execution_release(void);
#endif
