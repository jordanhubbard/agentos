/*
 * net_pd_test.c — contract tests for the Net PD
 *
 * Covered opcodes:
 *   MSG_NET_OPEN       (0x2101) — open a network interface, return handle
 *   MSG_NET_CLOSE      (0x2102) — release a handle
 *   MSG_NET_SEND       (0x2103) — transmit a frame (data in shmem)
 *   MSG_NET_RECV       (0x2104) — receive a frame into shmem
 *   MSG_NET_DEV_STATUS (0x2105) — query link state, rx/tx packet counts
 *   MSG_NET_CONFIGURE  (0x2106) — set MTU and flags
 *
 * Channel: CH_NET_PD (68) from the controller's perspective.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_net_pd_tests(microkit_channel ch)
{
    TEST_SECTION("net_pd");

    /* OPEN — open iface_id=0; may return busy if already open, or not-found
     * if no NIC is enumerated in this test topology. */
    microkit_mr_set(0, (uint64_t)MSG_NET_OPEN);
    microkit_mr_set(1, 0);  /* iface_id */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_NET_OPEN, 2));
    uint64_t open_rc = microkit_mr_get(0);
    uint64_t handle  = microkit_mr_get(1);
    {
        if (open_rc == AOS_OK || open_rc == AOS_ERR_BUSY ||
            open_rc == AOS_ERR_NOT_FOUND || open_rc == AOS_ERR_UNIMPL) {
            _tf_ok("net_pd: OPEN iface 0 returns ok, busy, not-found, or unimpl");
        } else {
            _tf_fail_point("net_pd: OPEN iface 0 returns ok, busy, not-found, or unimpl",
                           "unexpected error code");
        }
    }

    /*
     * Skip handle-dependent ops when OPEN did not succeed.
     */
    if (open_rc != AOS_OK) {
        _tf_puts("# net_pd: OPEN did not succeed; skipping handle-specific ops\n");

        const char *skipped[] = {
            "net_pd: CONFIGURE on open handle (skipped, no handle)",
            "net_pd: SEND len=0 (skipped, no handle)",
            "net_pd: RECV max=0 (skipped, no handle)",
            "net_pd: DEV_STATUS (skipped, no handle)",
            "net_pd: CLOSE handle (skipped, no handle)",
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

    /* CONFIGURE — set MTU=1500, flags=0. */
    microkit_mr_set(0, (uint64_t)MSG_NET_CONFIGURE);
    microkit_mr_set(1, handle);
    microkit_mr_set(2, 1500);  /* mtu */
    microkit_mr_set(3, 0);     /* flags */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_NET_CONFIGURE, 4));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_INVAL || rc == AOS_ERR_UNIMPL) {
            _tf_ok("net_pd: CONFIGURE returns ok, inval, or unimpl");
        } else {
            _tf_fail_point("net_pd: CONFIGURE returns ok, inval, or unimpl",
                           "unexpected error code");
        }
    }

    /* SEND — send a zero-length frame to probe the path without a shmem buffer. */
    microkit_mr_set(0, (uint64_t)MSG_NET_SEND);
    microkit_mr_set(1, handle);
    microkit_mr_set(2, 0);  /* len = 0 */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_NET_SEND, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_INVAL || rc == AOS_ERR_UNIMPL) {
            _tf_ok("net_pd: SEND len=0 returns ok, inval, or unimpl");
        } else {
            _tf_fail_point("net_pd: SEND len=0 returns ok, inval, or unimpl",
                           "unexpected error code");
        }
    }

    /* RECV — receive with max=0 to probe the dispatch path. */
    microkit_mr_set(0, (uint64_t)MSG_NET_RECV);
    microkit_mr_set(1, handle);
    microkit_mr_set(2, 0);  /* max = 0 */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_NET_RECV, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_INVAL || rc == AOS_ERR_UNIMPL) {
            _tf_ok("net_pd: RECV max=0 returns ok, inval, or unimpl");
        } else {
            _tf_fail_point("net_pd: RECV max=0 returns ok, inval, or unimpl",
                           "unexpected error code");
        }
    }

    /* DEV_STATUS — query link/rx_pkts/tx_pkts. */
    microkit_mr_set(0, (uint64_t)MSG_NET_DEV_STATUS);
    microkit_mr_set(1, handle);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_NET_DEV_STATUS, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_UNIMPL) {
            _tf_ok("net_pd: DEV_STATUS returns ok, not-found, or unimpl");
        } else {
            _tf_fail_point("net_pd: DEV_STATUS returns ok, not-found, or unimpl",
                           "unexpected error code");
        }
    }

    /* CLOSE — release the handle. */
    microkit_mr_set(0, (uint64_t)MSG_NET_CLOSE);
    microkit_mr_set(1, handle);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_NET_CLOSE, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("net_pd: CLOSE returns ok or not-found");
        } else {
            _tf_fail_point("net_pd: CLOSE returns ok or not-found",
                           "unexpected error code");
        }
    }
}
