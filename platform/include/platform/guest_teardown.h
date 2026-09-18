#ifndef AOS_PLATFORM_GUEST_TEARDOWN_H
#define AOS_PLATFORM_GUEST_TEARDOWN_H

#include <stdbool.h>
#include <stddef.h>

/* ARM terminal teardown. The caller must stop guest execution and enter
 * DESTROYING first. False requires another call while servicing device
 * completions; execution must never resume. Retains RAM until every backend
 * has relinquished its references. Does not release VSpace or service grants.
 * Reconstruction must explicitly reset this state before admitting a guest. */
typedef struct aos_guest_teardown {
    bool devices_quiesced;
    bool execution_released;
    bool ram_released;
} aos_guest_teardown_t;

bool aos_guest_teardown_step(aos_guest_teardown_t *state, size_t ram_size);

#endif
