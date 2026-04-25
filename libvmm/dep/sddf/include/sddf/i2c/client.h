/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <sel4/sel4.h>

/* By default we assume a 7-bit bus address. */
#ifndef I2C_BUS_ADDRESS_MAX
#define I2C_BUS_ADDRESS_MAX (0x7f)
#endif

#define I2C_BUS_SLOT (0)

/* Protected-Procedure-Call function idenitifers */
#define I2C_BUS_CLAIM       (1)
#define I2C_BUS_RELEASE     (2)

/* This is the label of the PPC response from the virtualiser */
#define I2C_SUCCESS (0)
#define I2C_FAILURE (1)

/* Helpers for interacting with the virutaliser. */
static inline bool i2c_bus_claim(seL4_CPtr virt_ep, size_t bus_address)
{
    seL4_SetMR(I2C_BUS_SLOT, bus_address);
    seL4_MessageInfo_t msginfo = seL4_Call(virt_ep, seL4_MessageInfo_new(I2C_BUS_CLAIM, 0, 0, 1));

    return seL4_MessageInfo_get_label(msginfo) == I2C_SUCCESS;
}

static inline bool i2c_bus_release(seL4_CPtr virt_ep, size_t bus_address)
{
    seL4_SetMR(I2C_BUS_SLOT, bus_address);
    seL4_MessageInfo_t msginfo = seL4_Call(virt_ep, seL4_MessageInfo_new(I2C_BUS_RELEASE, 0, 0, 1));

    return seL4_MessageInfo_get_label(msginfo) == I2C_SUCCESS;
}
