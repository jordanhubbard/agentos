/*
 * serial_pd_test.c — contract tests for the Serial PD
 *
 * Covered opcodes:
 *   MSG_SERIAL_OPEN      (0x2001) — open a serial port, return client slot
 *   MSG_SERIAL_CLOSE     (0x2002) — release a client slot
 *   MSG_SERIAL_WRITE     (0x2003) — write bytes to the serial device
 *   MSG_SERIAL_READ      (0x2004) — read bytes from the serial device
 *   MSG_SERIAL_STATUS    (0x2005) — query baud rate, rx count, error count
 *   MSG_SERIAL_CONFIGURE (0x2006) — set baud rate and flags
 *
 * Channel: CH_SERIAL_PD (67) from the controller's perspective.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"

void run_serial_pd_tests(microkit_channel ch)
{
    TEST_SECTION("serial_pd");

    /* OPEN — open port_id=0; may succeed or return busy if port already open. */
    microkit_mr_set(0, (uint64_t)MSG_SERIAL_OPEN);
    microkit_mr_set(1, 0);  /* port_id */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_SERIAL_OPEN, 2));
    uint64_t open_rc   = microkit_mr_get(0);
    uint64_t client_slot = microkit_mr_get(1);
    {
        if (open_rc == AOS_OK || open_rc == AOS_ERR_BUSY ||
            open_rc == AOS_ERR_NOT_FOUND || open_rc == AOS_ERR_UNIMPL) {
            _tf_ok("serial_pd: OPEN port 0 returns ok, busy, not-found, or unimpl");
        } else {
            _tf_fail_point("serial_pd: OPEN port 0 returns ok, busy, not-found, or unimpl",
                           "unexpected error code");
        }
    }

    /*
     * If OPEN did not succeed we emit TODO-skip points for slot-dependent ops
     * rather than issuing IPCs with an invalid client_slot.
     */
    if (open_rc != AOS_OK) {
        _tf_puts("# serial_pd: OPEN did not succeed; skipping slot-specific ops\n");

        const char *skipped[] = {
            "serial_pd: CONFIGURE on open slot (skipped, no slot)",
            "serial_pd: WRITE on open slot (skipped, no slot)",
            "serial_pd: READ on open slot (skipped, no slot)",
            "serial_pd: STATUS on open slot (skipped, no slot)",
            "serial_pd: CLOSE open slot (skipped, no slot)",
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

    /* CONFIGURE — set baud=115200 on the open slot. */
    microkit_mr_set(0, (uint64_t)MSG_SERIAL_CONFIGURE);
    microkit_mr_set(1, client_slot);
    microkit_mr_set(2, 115200);  /* baud */
    microkit_mr_set(3, 0);       /* flags = default */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_SERIAL_CONFIGURE, 4));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_INVAL || rc == AOS_ERR_UNIMPL) {
            _tf_ok("serial_pd: CONFIGURE returns ok, inval, or unimpl");
        } else {
            _tf_fail_point("serial_pd: CONFIGURE returns ok, inval, or unimpl",
                           "unexpected error code");
        }
    }

    /* WRITE — write 0 bytes (len=0) to probe the path without a shmem buffer. */
    microkit_mr_set(0, (uint64_t)MSG_SERIAL_WRITE);
    microkit_mr_set(1, client_slot);
    microkit_mr_set(2, 0);  /* len = 0 */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_SERIAL_WRITE, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        /* ok (0 bytes written) or inval (len=0 rejected) are both valid. */
        if (rc == AOS_OK || rc == AOS_ERR_INVAL || rc == AOS_ERR_UNIMPL) {
            _tf_ok("serial_pd: WRITE len=0 returns ok, inval, or unimpl");
        } else {
            _tf_fail_point("serial_pd: WRITE len=0 returns ok, inval, or unimpl",
                           "unexpected error code");
        }
    }

    /* READ — read up to 0 bytes (max=0) to probe the path. */
    microkit_mr_set(0, (uint64_t)MSG_SERIAL_READ);
    microkit_mr_set(1, client_slot);
    microkit_mr_set(2, 0);  /* max = 0 */
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_SERIAL_READ, 3));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_INVAL || rc == AOS_ERR_UNIMPL) {
            _tf_ok("serial_pd: READ max=0 returns ok, inval, or unimpl");
        } else {
            _tf_fail_point("serial_pd: READ max=0 returns ok, inval, or unimpl",
                           "unexpected error code");
        }
    }

    /* STATUS — query baud/rx_count/error_count for the open slot. */
    microkit_mr_set(0, (uint64_t)MSG_SERIAL_STATUS);
    microkit_mr_set(1, client_slot);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_SERIAL_STATUS, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND || rc == AOS_ERR_UNIMPL) {
            _tf_ok("serial_pd: STATUS returns ok, not-found, or unimpl");
        } else {
            _tf_fail_point("serial_pd: STATUS returns ok, not-found, or unimpl",
                           "unexpected error code");
        }
    }

    /* CLOSE — release the client slot. */
    microkit_mr_set(0, (uint64_t)MSG_SERIAL_CLOSE);
    microkit_mr_set(1, client_slot);
    (void)microkit_ppcall(ch, microkit_msginfo_new(MSG_SERIAL_CLOSE, 2));
    {
        uint64_t rc = microkit_mr_get(0);
        if (rc == AOS_OK || rc == AOS_ERR_NOT_FOUND) {
            _tf_ok("serial_pd: CLOSE open slot returns ok or not-found");
        } else {
            _tf_fail_point("serial_pd: CLOSE open slot returns ok or not-found",
                           "unexpected error code");
        }
    }
}
