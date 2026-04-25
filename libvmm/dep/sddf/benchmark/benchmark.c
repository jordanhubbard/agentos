/*
 * Copyright 2022, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdint.h>
#include <sel4/sel4.h>
#include <sel4/benchmark_track_types.h>
#include <sel4/benchmark_utilisation_types.h>
#include <sddf/benchmark/sel4bench.h>
#include <sddf/benchmark/config.h>
#include <sddf/serial/queue.h>
#include <sddf/serial/config.h>
#include <sddf/util/fence.h>
#include <sddf/util/util.h>
#include <sddf/util/printf.h>

#define LOG_BUFFER_CAP 7

__attribute__((__section__(".benchmark_config"))) benchmark_config_t benchmark_config;

__attribute__((__section__(".serial_client_config"))) serial_client_config_t serial_config;

ccnt_t counter_values[8];
counter_bitfield_t benchmark_bf;

serial_queue_handle_t serial_tx_queue_handle;

char *counter_names[] = {
    "L1 i-cache misses",
    "L1 d-cache misses",
    "L1 i-tlb misses",
    "L1 d-tlb misses",
    "Instructions",
    "Branch mispredictions",
};

event_id_t benchmarking_events[] = {
    SEL4BENCH_EVENT_CACHE_L1I_MISS,
    SEL4BENCH_EVENT_CACHE_L1D_MISS,
    SEL4BENCH_EVENT_TLB_L1I_MISS,
    SEL4BENCH_EVENT_TLB_L1D_MISS,
    SEL4BENCH_EVENT_EXECUTE_INSTRUCTION,
    SEL4BENCH_EVENT_BRANCH_MISPREDICT,
};

static char *child_name(uint8_t child_id)
{
    for (uint8_t i = 0; i < benchmark_config.num_children; i++) {
        if (child_id == benchmark_config.children[i].child_id) {
            return benchmark_config.children[i].name;
        }
    }
    return "unknown child PD";
}

#ifdef CONFIG_BENCHMARK_TRACK_UTILISATION
static void print_total_util(uint64_t *buffer)
{
    uint64_t total = buffer[BENCHMARK_TOTAL_UTILISATION];
    uint64_t number_schedules = buffer[BENCHMARK_TOTAL_NUMBER_SCHEDULES];
    uint64_t kernel = buffer[BENCHMARK_TOTAL_KERNEL_UTILISATION];
    uint64_t entries = buffer[BENCHMARK_TOTAL_NUMBER_KERNEL_ENTRIES];
    sddf_printf("Total utilisation details: \n{\nKernelUtilisation: %lu\nKernelEntries: %lu\nNumberSchedules: "
                "%lu\nTotalUtilisation: %lu\n}\n",
                kernel, entries, number_schedules, total);
}

static void print_child_util(uint64_t *buffer, uint8_t id)
{
    uint64_t total = buffer[BENCHMARK_TCB_UTILISATION];
    uint64_t number_schedules = buffer[BENCHMARK_TCB_NUMBER_SCHEDULES];
    uint64_t kernel = buffer[BENCHMARK_TCB_KERNEL_UTILISATION];
    uint64_t entries = buffer[BENCHMARK_TCB_NUMBER_KERNEL_ENTRIES];
    sddf_printf("Utilisation details for PD: %s (%u)\n{\nKernelUtilisation: %lu\nKernelEntries: %lu\nNumberSchedules: "
                "%lu\nTotalUtilisation: %lu\n}\n",
                child_name(id), id, kernel, entries, number_schedules, total);
}

#endif

#ifdef CONFIG_BENCHMARK_TRACK_KERNEL_ENTRIES
benchmark_track_kernel_entry_t *log_buffer;

static void dump_log_summary(uint64_t log_size)
{
    seL4_Word index = 0;
    seL4_Word syscall_entries = 0;
    seL4_Word fastpaths = 0;
    seL4_Word interrupt_entries = 0;
    seL4_Word userlevelfault_entries = 0;
    seL4_Word vmfault_entries = 0;
    seL4_Word debug_fault = 0;
    seL4_Word other = 0;

    while (log_buffer[index].start_time != 0 && index < log_size) {
        if (log_buffer[index].entry.path == Entry_Syscall) {
            if (log_buffer[index].entry.is_fastpath) {
                fastpaths++;
            }
            syscall_entries++;
        } else if (log_buffer[index].entry.path == Entry_Interrupt) {
            interrupt_entries++;
        } else if (log_buffer[index].entry.path == Entry_UserLevelFault) {
            userlevelfault_entries++;
        } else if (log_buffer[index].entry.path == Entry_VMFault) {
            vmfault_entries++;
        } else if (log_buffer[index].entry.path == Entry_DebugFault) {
            debug_fault++;
        } else {
            other++;
        }

        index++;
    }

    sddf_printf("System call invocations %lu", syscall_entries);
    sddf_printf("Fastpaths %lu\n", fastpaths);
    sddf_printf("Interrupt invocations %lu\n", interrupt_entries);
    sddf_printf("User-level faults %lu\n", userlevelfault_entries);
    sddf_printf("VM faults %lu\n", vmfault_entries);
    sddf_printf("Debug faults %lu\n", debug_fault);
    sddf_printf("Others %lu\n", other);
}
#endif

static void benchmark_init(void)
{
#if !ENABLE_BENCHMARKING
    sddf_dprintf("BENCH|LOG: Bench running in debug mode, no access to counters\n");
    return;
#endif

#if ENABLE_PMU_EVENTS
    sel4bench_init();
    seL4_Word n_counters = sel4bench_get_num_counters();
    for (seL4_Word counter = 0; counter < MIN(n_counters, ARRAY_SIZE(benchmarking_events)); counter++) {
        sel4bench_set_count_event(counter, benchmarking_events[counter]);
        benchmark_bf |= BIT(counter);
    }

    sel4bench_reset_counters();
    sel4bench_start_counters(benchmark_bf);
#endif

#ifdef CONFIG_BENCHMARK_TRACK_KERNEL_ENTRIES
    int res_buf = seL4_BenchmarkSetLogBuffer(LOG_BUFFER_CAP);
    if (res_buf) {
        sddf_printf("BENCH|ERROR: Could not set log buffer: %d\n", res_buf);
    } else {
        sddf_printf("BENCH|LOG: Log buffer set\n");
    }
#endif
}

static void benchmark_start(void)
{
#if !ENABLE_BENCHMARKING
    sddf_printf("BENCHMARK: benchmark_start is no-op as benchmarking is disabled\n");
    return;
#endif

#if ENABLE_PMU_EVENTS
    sel4bench_reset_counters();
    THREAD_MEMORY_RELEASE();
    sel4bench_start_counters(benchmark_bf);
#endif

#ifdef CONFIG_BENCHMARK_TRACK_UTILISATION
    seL4_BenchmarkResetThreadUtilisation(TCB_CAP);
    for (uint8_t i = 0; i < benchmark_config.num_children; i++) {
        seL4_BenchmarkResetThreadUtilisation(BASE_TCB_CAP + benchmark_config.children[i].child_id);
    }
    seL4_BenchmarkResetLog();
#endif

#ifdef CONFIG_BENCHMARK_TRACK_KERNEL_ENTRIES
    seL4_BenchmarkResetLog();
#endif

    /* Notify benchmark PD running on next core */
    if (!benchmark_config.last_core) {
        seL4_Signal(benchmark_config.tx_start_ch);
    }
}

static void benchmark_stop(void)
{
#if !ENABLE_BENCHMARKING
    sddf_printf("BENCHMARK: benchmark_stop is no-op as benchmarking is disabled\n");
    return;
#endif

#if ENABLE_PMU_EVENTS
    sel4bench_get_counters(benchmark_bf, &counter_values[0]);
    sel4bench_stop_counters(benchmark_bf);

    sddf_printf("{CORE %u: \n", benchmark_config.core);
    for (int i = 0; i < ARRAY_SIZE(benchmarking_events); i++) {
        sddf_printf("%s: %lu\n", counter_names[i], counter_values[i]);
    }
    sddf_printf("}\n");
#endif

#ifdef CONFIG_BENCHMARK_TRACK_UTILISATION
    seL4_BenchmarkFinalizeLog();
    seL4_BenchmarkGetThreadUtilisation(TCB_CAP);
    print_total_util((uint64_t *)&seL4_GetIPCBuffer()->msg[0]);
    for (uint8_t i = 0; i < benchmark_config.num_children; i++) {
        seL4_BenchmarkGetThreadUtilisation(BASE_TCB_CAP + benchmark_config.children[i].child_id);
        print_child_util((uint64_t *)&seL4_GetIPCBuffer()->msg[0], benchmark_config.children[i].child_id);
    }
#endif

#ifdef CONFIG_BENCHMARK_TRACK_KERNEL_ENTRIES
    uint64_t entries = seL4_BenchmarkFinalizeLog();
    sddf_printf("KernelEntries:  %lu\n", entries);
    dump_log_summary(entries);
#endif

    /* Notify benchmark PD running on next core */
    if (!benchmark_config.last_core) {
        seL4_Signal(benchmark_config.tx_stop_ch);
    }
}

static seL4_CPtr g_ep;

static void pd_notified(seL4_Word badge)
{
    seL4_Word ch = badge;
    if (ch == serial_config.tx.id) {
        return;
    } else if (ch == benchmark_config.rx_start_ch) {
        benchmark_start();
    } else if (ch == benchmark_config.rx_stop_ch) {
        benchmark_stop();
    } else {
        sddf_printf("BENCH|LOG: Bench thread notified on unexpected channel %lu\n", ch);
    }
}

void benchmark_main(seL4_CPtr ep)
{
    g_ep = ep;

    serial_queue_init(&serial_tx_queue_handle, serial_config.tx.queue.vaddr, serial_config.tx.data.size,
                      serial_config.tx.data.vaddr);
    serial_putchar_init(serial_config.tx.id, &serial_tx_queue_handle);

#if ENABLE_BENCHMARKING
    sddf_printf("BENCH|LOG: ENABLE_BENCHMARKING defined\n");
#endif
#if ENABLE_PMU_EVENTS
    sddf_printf("BENCH|LOG: ENABLE_PMU_EVENTS defined\n");
#endif
#ifdef CONFIG_BENCHMARK_TRACK_UTILISATION
    sddf_printf("BENCH|LOG: CONFIG_BENCHMARK_TRACK_UTILISATION defined\n");
#endif
#ifdef CONFIG_BENCHMARK_TRACK_KERNEL_ENTRIES
    sddf_printf("BENCH|LOG: CONFIG_BENCHMARK_TRACK_KERNEL_ENTRIES defined\n");
#endif

    benchmark_init();
    seL4_Signal(benchmark_config.init_ch);

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
