/*
 * Copyright 2025, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#include <sel4/sel4.h>
#include <stdint.h>

typedef seL4_CPtr sddf_channel;

static inline void sddf_irq_ack(sddf_channel ch)
{
    seL4_IRQHandler_Ack(ch);
}

static inline void sddf_deferred_irq_ack(sddf_channel ch)
{
    seL4_IRQHandler_Ack(ch);
}

static inline void sddf_notify(sddf_channel ch)
{
    seL4_Signal(ch);
}

static inline void sddf_deferred_notify(sddf_channel ch)
{
    seL4_Signal(ch);
}

static inline seL4_MessageInfo_t sddf_ppcall(sddf_channel ch, seL4_MessageInfo_t msginfo)
{
    return seL4_Call(ch, msginfo);
}

static inline uint64_t sddf_get_mr(unsigned int n)
{
    return seL4_GetMR(n);
}

static inline void sddf_set_mr(unsigned int n, uint64_t val)
{
    seL4_SetMR(n, val);
}
