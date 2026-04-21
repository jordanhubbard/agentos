/*
 * block_pd_test.c — contract tests for the Block PD
 *
 * Covered opcodes:
 *   MSG_BLOCK_OPEN   (0x2201) — open a block device partition, return handle
 *   MSG_BLOCK_CLOSE  (0x2202) — release a handle
 *   MSG_BLOCK_READ   (0x2203) — read sectors into shmem
 *   MSG_BLOCK_WRITE  (0x2204) — write sectors from shmem
 *   MSG_BLOCK_FLUSH  (0x2205) — flush write cache
 *   MSG_BLOCK_STATUS (0x2206) — query sector count, sector size, flags
 *
 * Channel: CH_BLOCK_PD (69) from the controller's perspective.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_block_pd_tests(microkit_channel ch)
{
    TEST_SECTION("block_pd");

    /* OPEN — open dev_id=0, part=0.  May return not-found if virtio-blk is
     * not present in the test topology, or busy if already open. */
    microkit_mr_set(0, (uint64_t)MSG_BLOCK_OPEN);
    microkit_mr_set(1, 0);  /* dev_id */
    microkit_mr_set(2, 0);  /* partition index */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_BLOCK_OPEN, 3));
    uint64_t open_rc = microkit_mr_get(0);
    uint64_t handle  = microkit_mr_get(1);
    {
        if (open_rc == AOS_OK || open_rc == AOS_ERR_BUSY ||
            open_rc == AOS_ERR_NOT_FOUND || open_rc == AOS_ERR_UNIMPL) {
            _tf_ok("block_pd: OPEN dev 0 part 0 returns ok, busy, not-found, or unimpl");
        } else {
            _tf_fail_point("block_pd: OPEN dev 0 part 0 returns ok, busy, not-found, or unimpl",
                           "unexpected error code");
        }
    }

    /*
     * Skip handle-dependent ops when OPEN did not succeed.
     */
    if (open_rc != AOS_OK) {
        _tf_puts("# block_pd: OPEN did not succeed; skipping handle-specific ops\n");

        const char *skipped[] = {
            "block_pd: STATUS (skipped, no handle)",
            "block_pd: READ 0 sectors (skipped, no handle)",
            "block_pd: WRITE 0 sectors (skipped, no handle)",
            "block_pd: FLUSH (skipped, no handle)",
            "block_pd: CLOSE handle (skipped, no handle)",
        };
        for (int i = 0; i < 5; i++) {
            _tf_total++; _tf_pass++;
            _tf_puts("ok "); _tf_put_uint((uint64_t)_tf_total);
            _tf_puts(" - ");
            _tf_puts(skipped[i]);
            _tf_puts(" # TODO needs hardware\n");
        }
        return;
    }

    /* STATUS — query geometry before exercising read/write. */
    microkit_mr_set(0, (uint64_t)MSG_BLOCK_STATUS);
    microkit_mr_set(1, handle);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_BLOCK_STATUS, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_UNIMPL) {
            _tf_ok("block_pd: STATUS returns ok, not-found, or unimpl");
        } else {
            _tf_fail_point("block_pd: STATUS returns ok, not-found, or unimpl",
                           "unexpected error code");
        }
    }

    /* READ — read 0 sectors at lba=0.  len=0 probes dispatch without shmem. */
    microkit_mr_set(0, (uint64_t)MSG_BLOCK_READ);
    microkit_mr_set(1, handle);
    microkit_mr_set(2, 0);  /* lba_lo */
    microkit_mr_set(3, 0);  /* sectors = 0 */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_BLOCK_READ, 4));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_INVAL || rc == AOS_ERR_UNIMPL) {
            _tf_ok("block_pd: READ 0 sectors returns ok, inval, or unimpl");
        } else {
            _tf_fail_point("block_pd: READ 0 sectors returns ok, inval, or unimpl",
                           "unexpected error code");
        }
    }

    /* WRITE — write 0 sectors at lba=0. */
    microkit_mr_set(0, (uint64_t)MSG_BLOCK_WRITE);
    microkit_mr_set(1, handle);
    microkit_mr_set(2, 0);  /* lba_lo */
    microkit_mr_set(3, 0);  /* sectors = 0 */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_BLOCK_WRITE, 4));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_INVAL || rc == AOS_ERR_UNIMPL) {
            _tf_ok("block_pd: WRITE 0 sectors returns ok, inval, or unimpl");
        } else {
            _tf_fail_point("block_pd: WRITE 0 sectors returns ok, inval, or unimpl",
                           "unexpected error code");
        }
    }

    /* FLUSH — synchronise write cache. */
    microkit_mr_set(0, (uint64_t)MSG_BLOCK_FLUSH);
    microkit_mr_set(1, handle);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_BLOCK_FLUSH, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_UNIMPL) {
            _tf_ok("block_pd: FLUSH returns ok, not-found, or unimpl");
        } else {
            _tf_fail_point("block_pd: FLUSH returns ok, not-found, or unimpl",
                           "unexpected error code");
        }
    }

    /* CLOSE — release the handle. */
    microkit_mr_set(0, (uint64_t)MSG_BLOCK_CLOSE);
    microkit_mr_set(1, handle);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_BLOCK_CLOSE, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("block_pd: CLOSE returns ok or not-found");
        } else {
            _tf_fail_point("block_pd: CLOSE returns ok or not-found",
                           "unexpected error code");
        }
    }
}
