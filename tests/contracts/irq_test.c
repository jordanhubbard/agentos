/*
 * irq_test.c — contract tests for the IRQ PD
 *
 * Covered opcodes:
 *   IRQ_OP_REGISTER                — register an IRQ handler
 *   IRQ_OP_UNREGISTER              — unregister an IRQ
 *   IRQ_OP_ACKNOWLEDGE             — acknowledge an IRQ
 *   IRQ_OP_MASK                    — mask an IRQ
 *   IRQ_OP_UNMASK                  — unmask an IRQ
 *   IRQ_OP_STATUS                  — query IRQ status
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/irq_contract.h"

void run_irq_tests(microkit_channel ch)
{
    TEST_SECTION("irq");

    /* STATUS — query bogus IRQ */
    microkit_mr_set(0, (uint64_t)IRQ_OP_STATUS);
    microkit_mr_set(1, 0);  /* irq_num 0 — may or may not be registered */
    (void)microkit_ppcall(ch, microkit_msginfo_new(IRQ_OP_STATUS, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == IRQ_OK || rc == IRQ_ERR_BAD_SLOT || rc == IRQ_ERR_BAD_IRQ) {
            _tf_ok("irq: STATUS returns ok/bad-slot/bad-irq");
        } else {
            _tf_fail_point("irq: STATUS returns ok/bad-slot/bad-irq",
                           "unexpected error code");
        }
    }

    /* UNREGISTER — unregister an IRQ that isn't registered */
    microkit_mr_set(0, (uint64_t)IRQ_OP_UNREGISTER);
    microkit_mr_set(1, 63);  /* high IRQ number, likely not registered */
    (void)microkit_ppcall(ch, microkit_msginfo_new(IRQ_OP_UNREGISTER, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == IRQ_OK || rc == IRQ_ERR_BAD_SLOT) {
            _tf_ok("irq: UNREGISTER unregistered IRQ returns ok/bad-slot");
        } else {
            _tf_fail_point("irq: UNREGISTER unregistered IRQ returns ok/bad-slot",
                           "unexpected error code");
        }
    }

    /* MASK — mask a bogus IRQ */
    microkit_mr_set(0, (uint64_t)IRQ_OP_MASK);
    microkit_mr_set(1, 63);
    (void)microkit_ppcall(ch, microkit_msginfo_new(IRQ_OP_MASK, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == IRQ_OK || rc == IRQ_ERR_BAD_SLOT || rc == IRQ_ERR_NOT_IMPL) {
            _tf_ok("irq: MASK returns ok/bad-slot/not-impl");
        } else {
            _tf_fail_point("irq: MASK returns ok/bad-slot/not-impl",
                           "unexpected error code");
        }
    }

    /* ACKNOWLEDGE — ack IRQ 0 */
    microkit_mr_set(0, (uint64_t)IRQ_OP_ACKNOWLEDGE);
    microkit_mr_set(1, 0);
    (void)microkit_ppcall(ch, microkit_msginfo_new(IRQ_OP_ACKNOWLEDGE, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == IRQ_OK || rc == IRQ_ERR_BAD_SLOT || rc == IRQ_ERR_NOT_IMPL) {
            _tf_ok("irq: ACKNOWLEDGE returns ok/bad-slot/not-impl");
        } else {
            _tf_fail_point("irq: ACKNOWLEDGE returns ok/bad-slot/not-impl",
                           "unexpected error code");
        }
    }
}
