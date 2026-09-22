/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef AOS_INPUT_REBIND_H
#define AOS_INPUT_REBIND_H
#include <stdbool.h>
#include <platform/input.h>
#include "contracts/input_virt_contract.h"
/* Validation touches private state and frontend cursors only; it never
 * dereferences a detached client's former frame. Drain old requests before
 * admitting a new generation, so queued old presses cannot reach it.
 * The caller serializes validation, fresh frame setup, and commit with pump.
 * Capability transfer/mapping must finish before commit; failure leaves the
 * client retired. No queue is cleared by commit. */
uint32_t aos_input_rebind_validate(const aos_input_service_t *, uint64_t badge,
    const input_virt_rebind_req_t *, size_t length);
uint32_t aos_input_rebind_commit(aos_input_service_t *, uint64_t badge,
    const input_virt_rebind_req_t *, size_t length, aos_input_client_region_t *fresh);
static inline bool aos_input_rebind_reply_valid(const input_virt_rebind_reply_t *r,
    size_t length, uint32_t generation)
{
    return r && length==sizeof(*r) && generation &&
        r->status==AOS_INPUT_OK && r->version==INPUT_VIRT_REBIND_VERSION &&
        r->generation==generation;
}
/* Call only after terminal detach and pool revocation with an empty receive
 * slot. Success maps the new frame at this client's canonical VA. Failure
 * leaves execution stopped; a service-committed frame may require detach
 * and pool revocation before a new generation can be attempted. */
bool aos_input_virt_rebind(uint32_t client, uint32_t generation);
uint32_t aos_input_rebind_retire(aos_input_service_t *,uint64_t badge,
    const input_virt_rebind_req_t *,size_t length);
bool aos_input_virt_retire(uint32_t client,uint32_t generation);
#endif
