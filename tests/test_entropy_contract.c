/* Host test: entropy request contract validation. No seL4; logic only. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "contracts/entropy_contract.h"

int main(void)
{
    aos_entropy_req_t r;

    /* A well-formed request is accepted. */
    memset(&r, 0, sizeof(r));
    r.version = AOS_ENTROPY_VERSION;
    r.length  = 16u;
    assert(aos_entropy_validate_req(&r) == AOS_ENTROPY_OK);

    /* Boundary: the maximum (32 bytes -- a 256-bit key/nonce, and the most
     * this driver's single-round-trip IPC reply can carry; see
     * entropy_contract.h) is accepted, one past it is not. */
    r.length = AOS_ENTROPY_MAX_BYTES;
    assert(aos_entropy_validate_req(&r) == AOS_ENTROPY_OK);
    r.length = AOS_ENTROPY_MAX_BYTES + 1u;
    assert(aos_entropy_validate_req(&r) == AOS_ENTROPY_ERR_RANGE);

    /* Zero length is a range error, not a silently-empty success. */
    r.length = 0u;
    assert(aos_entropy_validate_req(&r) == AOS_ENTROPY_ERR_RANGE);

    /* A version mismatch is rejected before the length is considered. */
    r.version = AOS_ENTROPY_VERSION + 1u;
    r.length  = 32u;
    assert(aos_entropy_validate_req(&r) == AOS_ENTROPY_ERR_VERSION);

    /* NULL is rejected rather than dereferenced. */
    assert(aos_entropy_validate_req(NULL) == AOS_ENTROPY_ERR_VERSION);

    /* Reserved fields must be zero: a caller setting them is rejected, so the
     * field stays available for a later version without an ABI break. */
    memset(&r, 0, sizeof(r));
    r.version  = AOS_ENTROPY_VERSION;
    r.length   = 16u;
    r.reserved = 1u;
    assert(aos_entropy_validate_req(&r) == AOS_ENTROPY_ERR_RANGE);

    printf("test_entropy_contract: PASS\n");
    return 0;
}
