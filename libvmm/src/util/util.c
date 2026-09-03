/*
 * Copyright 2022, UNSW (ABN 57 195 873 179)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sel4/sel4.h>
#include <libvmm/util/util.h>
#include <libvmm/util/printf.h>

/* Weak so linux_vmm can send printf to the mapped PL011 (release seL4 has
 * CONFIG_PRINTING off; seL4_DebugPutChar is then a no-op). */
__attribute__((weak)) void _putchar(char character)
{
    seL4_DebugPutChar(character);
}

void print_mem_hex(uintptr_t addr, size_t size)
{
#ifdef CONFIG_DEBUG_BUILD
    for (size_t i = 0; i < size; i++) {
        printf("%02X ", *((uint8_t *)addr + i));
        if ((i + 1) % 16 == 0) {
            printf("\n");
        }
    }
    printf("\n");
#endif
}
