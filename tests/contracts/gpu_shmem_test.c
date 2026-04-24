/*
 * gpu_shmem_test.c — contract tests for the GPUShmem PD
 *
 * Covered opcodes:
 *   MSG_GPUSHMEM_MAP               — map shared memory for GPU DMA
 *   MSG_GPUSHMEM_UNMAP             — unmap a slot
 *   MSG_GPUSHMEM_FENCE             — fence DMA completion
 *   MSG_GPUSHMEM_STATUS            — query shmem status
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_gpu_shmem_tests(microkit_channel ch)
{
    TEST_SECTION("gpu_shmem");

    /* STATUS — query slot usage */
    ASSERT_IPC_OK(ch, MSG_GPUSHMEM_STATUS, "gpu_shmem: STATUS returns ok");

    /* UNMAP — unmap bogus slot */
    microkit_mr_set(0, (uint64_t)MSG_GPUSHMEM_UNMAP);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_GPUSHMEM_UNMAP, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("gpu_shmem: UNMAP with bogus slot returns ok/not-found/inval");
        } else {
            _tf_fail_point("gpu_shmem: UNMAP with bogus slot returns ok/not-found/inval",
                           "unexpected error code");
        }
    }

    /* FENCE — fence bogus slot */
    microkit_mr_set(0, (uint64_t)MSG_GPUSHMEM_FENCE);
    microkit_mr_set(1, 0xFFFFu);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_GPUSHMEM_FENCE, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_INVAL) {
            _tf_ok("gpu_shmem: FENCE with bogus slot returns ok/not-found/inval");
        } else {
            _tf_fail_point("gpu_shmem: FENCE with bogus slot returns ok/not-found/inval",
                           "unexpected error code");
        }
    }

    /* MAP — map 1 page; expect ok or NOSPC */
    microkit_mr_set(0, (uint64_t)MSG_GPUSHMEM_MAP);
    microkit_mr_set(1, 1);  /* size_pages */
    microkit_mr_set(2, 0);  /* flags */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_GPUSHMEM_MAP, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOSPC || rc == AOS_ERR_UNIMPL) {
            _tf_ok("gpu_shmem: MAP returns ok/nospc/unimpl");
        } else {
            _tf_fail_point("gpu_shmem: MAP returns ok/nospc/unimpl",
                           "unexpected error code");
        }
    }
}
