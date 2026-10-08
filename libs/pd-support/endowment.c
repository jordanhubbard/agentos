/*
 * endowment.c -- subsetting validation for runtime domain creation.
 *
 * See kernel/agentos-root-task/include/contracts/endowment_contract.h for
 * the full contract. This file makes NO seL4 calls and includes NO seL4
 * headers: it is a pure, host-testable check that a requested endowment
 * never asks for more than the parent declares it holds. The parent
 * cannot mint from a capability it does not possess -- seL4 enforces
 * that for free -- but a malformed or over-reaching request should be
 * refused here, before any seL4 object is touched.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stddef.h>

#include "contracts/endowment_contract.h"

int aos_endowment_validate(const aos_endowment_t *req,
                            const aos_endowment_t *parent_holdings)
{
    if (req == NULL || parent_holdings == NULL) {
        return -1;
    }

    /* Version checked before anything else is examined. */
    if (req->version != AOS_ENDOWMENT_VERSION) {
        return -1;
    }

    /* An endowment granting nothing is a mistake, not a domain. */
    if (req->count == 0) {
        return -1;
    }

    /* Reject before reading past the array -- req is attacker-shaped
     * data in the general case. */
    if (req->count > AOS_ENDOWMENT_MAX_CAPS) {
        return -1;
    }

    for (uint32_t i = 0; i < req->count; i++) {
        const aos_endow_cap_t *rc = &req->caps[i];
        int found = 0;

        for (uint32_t j = 0; j < parent_holdings->count &&
                              j < AOS_ENDOWMENT_MAX_CAPS; j++) {
            const aos_endow_cap_t *pc = &parent_holdings->caps[j];

            if (pc->parent_slot != rc->parent_slot || pc->kind != rc->kind) {
                continue;
            }

            /* Subset test on the bitmask: every bit requested must
             * already be set in the parent's rights. Not equality
             * (legitimate narrowing must pass) and not "nonzero"
             * (any escalation bit must fail). */
            if ((rc->rights & ~pc->rights) != 0) {
                return -1;
            }

            found = 1;
            break;
        }

        if (!found) {
            return -1;
        }
    }

    return 0;
}
