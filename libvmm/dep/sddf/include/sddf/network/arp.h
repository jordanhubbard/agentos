/*
 * Copyright 2024, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <sel4/sel4.h>

bool arp_register_ipv4(seL4_CPtr arp_ep, uint32_t ipv4_addr, uint8_t mac[6])
{
    seL4_SetMR(0, ipv4_addr);
    seL4_SetMR(1, (mac[0] << 8) | mac[1]);
    seL4_SetMR(2, (mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5]);
    seL4_Call(arp_ep, seL4_MessageInfo_new(0, 0, 0, 3));

    return true;
}
