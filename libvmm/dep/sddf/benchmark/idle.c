/*
 * Copyright 2022, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdint.h>
#include <sel4/sel4.h>
#include <sddf/util/util.h>
#include <sddf/util/fence.h>
#include <sddf/util/printf.h>
#include <sddf/benchmark/sel4bench.h>
#include <sddf/benchmark/bench.h>
#include <sddf/benchmark/config.h>

#define MAGIC_CYCLES 150

__attribute__((__section__(".benchmark_config"))) benchmark_idle_config_t config;

struct bench *b;

static inline uint64_t read_cycle_count()
{
    uint64_t cycle_count;
#if defined(CONFIG_ARCH_ARM)
    SEL4BENCH_READ_CCNT(cycle_count);
#elif defined(CONFIG_ARCH_RISCV)
    asm volatile("rdcycle %0" : "=r"(cycle_count));
#elif defined(CONFIG_ARCH_X86_64)
    // Do nothing only for build atm
#else
#error "read_cycle_count: unsupported architecture"
#endif

    return cycle_count;
}

void count_idle(void)
{
#if ENABLE_BENCHMARKING
    b->prev_ccount = read_cycle_count();

    while (1) {
        __atomic_store_n(&b->core_ccount, read_cycle_count(), __ATOMIC_RELAXED);
        uint64_t diff = b->core_ccount - b->prev_ccount;

        if (diff < MAGIC_CYCLES) {
            __atomic_store_n(&b->idle_ccount, __atomic_load_n(&b->idle_ccount, __ATOMIC_RELAXED) + diff,
                             __ATOMIC_RELAXED);
        }

        b->prev_ccount = b->core_ccount;
    }
#endif
}

static seL4_CPtr g_ep;

static void pd_notified(seL4_Word badge)
{
    seL4_Word ch = badge;
    if (ch == config.init_channel) {
        count_idle();
    } else {
        sddf_dprintf("Idle thread notified on unexpected channel: %lu\n", ch);
    }
}

void benchmark_idle_main(seL4_CPtr ep)
{
    g_ep = ep;

    b = (void *)config.cycle_counters;

    seL4_Word badge;
    while (1) {
        seL4_MessageInfo_t info = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(info);
        if (label == seL4_Fault_NullFault) {
            pd_notified(badge);
        } else {
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
        }
    }
}
