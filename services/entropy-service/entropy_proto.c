/*
 * Entropy service IPC contract: request validation.
 *
 * See kernel/agentos-root-task/include/contracts/entropy_contract.h for the
 * scope of what this service provides (provenance only -- no claim about
 * statistical quality, entropy estimation, or cryptographic suitability).
 */

#include <stddef.h>

#include "contracts/entropy_contract.h"

int aos_entropy_validate_req(const aos_entropy_req_t *req)
{
    if (req == NULL) {
        return AOS_ENTROPY_ERR_VERSION;
    }

    if (req->version != AOS_ENTROPY_VERSION) {
        return AOS_ENTROPY_ERR_VERSION;
    }

    if (req->reserved != 0u) {
        return AOS_ENTROPY_ERR_RANGE;
    }

    if (req->length == 0u || req->length > AOS_ENTROPY_MAX_BYTES) {
        return AOS_ENTROPY_ERR_RANGE;
    }

    return AOS_ENTROPY_OK;
}
