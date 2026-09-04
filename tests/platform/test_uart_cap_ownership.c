/*
 * PL011 capability ownership policy.
 * Host pre-filter for the production AArch64 topology and source wiring.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "system_desc.h"

#ifndef AOS_REPO_ROOT
#define AOS_REPO_ROOT "./"
#endif

extern const system_desc_t system_desc_aarch64;

static int failures;
static int tests;

static void ok(int condition, const char *name)
{
    tests++;
    printf("%s %d - %s\n", condition ? "ok" : "not ok", tests, name);
    if (!condition) {
        failures++;
    }
}

static const pd_desc_t *find_pd(const char *name)
{
    for (uint32_t i = 0u; i < system_desc_aarch64.pd_count; i++) {
        if (strcmp(system_desc_aarch64.pds[i].name, name) == 0) {
            return &system_desc_aarch64.pds[i];
        }
    }
    return NULL;
}

static char *read_source(const char *relative)
{
    char path[1024];
    FILE *file;
    long size;
    char *text;

    snprintf(path, sizeof(path), "%s%s", AOS_REPO_ROOT, relative);
    file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        if (file) fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || size > 4 * 1024 * 1024) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    text = malloc((size_t)size + 1u);
    if (!text) {
        fclose(file);
        return NULL;
    }
    size_t read_size = fread(text, 1u, (size_t)size, file);
    fclose(file);
    text[read_size] = '\0';
    return text;
}

static int source_contains(const char *relative, const char *needle)
{
    char *text = read_source(relative);
    int found = text && strstr(text, needle);
    free(text);
    return found;
}

int main(void)
{
    const pd_desc_t *serial = find_pd("serial_pd");
    unsigned uart_frames = 0u;
    unsigned uart_irqs = 0u;

    printf("TAP version 14\n");
    printf("# suite: uart_cap_ownership\n");

    for (uint32_t i = 0u; i < system_desc_aarch64.pd_count; i++) {
        const pd_desc_t *pd = &system_desc_aarch64.pds[i];
        for (uint8_t j = 0u; j < pd->device_frame_count; j++) {
            if (pd->device_frames[j].paddr == 0x09000000ULL) {
                uart_frames++;
                ok(strcmp(pd->name, "serial_pd") == 0,
                   "PL011 device-frame descriptor belongs only to serial_pd");
            }
        }
        for (uint8_t j = 0u; j < pd->irq_count; j++) {
            if (pd->irqs[j].irq_number == 33u) {
                uart_irqs++;
                ok(strcmp(pd->name, "serial_pd") == 0,
                   "IRQ 33 descriptor belongs only to serial_pd");
            }
        }
    }

    ok(serial != NULL, "serial_pd exists in AArch64 topology");
    ok(uart_frames == 1u, "exactly one PL011 device-frame assignment exists");
    ok(uart_irqs == 1u, "exactly one IRQ 33 assignment exists");

    ok(source_contains("boards/qemu-aarch64/board.mk",
                       "BOARD_UART_PHYS  := 0x09000000") &&
       source_contains("boards/qemu-aarch64/board.mk",
                       "BOARD_UART_IRQ   := 33"),
       "QEMU board metadata identifies the serial_pd PL011 resources");

    ok(source_contains("kernel/agentos-root-task/src/main.c",
                       "seL4_ARCH_Page_Unmap(g_uart_frame_cap)") &&
       source_contains("kernel/agentos-root-task/src/main.c",
                       "seL4_CNode_Move(") &&
       source_contains("kernel/agentos-root-task/src/main.c",
                       "g_uart_frame_cap = seL4_CapNull;"),
       "root unmaps and transfers its bounded bootstrap UART cap");

    ok(source_contains("kernel/agentos-root-task/src/main.c",
                       "IRQ cap move failed") &&
       !source_contains("kernel/agentos-root-task/src/main.c",
                        "IRQ cap copy failed"),
       "root moves IRQ handlers into owners without retaining aliases");

    ok(!source_contains("kernel/agentos-root-task/src/linux_vmm.c",
                        "LINUX_VMM_UART_VA") &&
       !source_contains("kernel/agentos-root-task/src/linux_vmm.c",
                        "g_uart_irq_cap") &&
       !source_contains("kernel/agentos-root-task/src/freebsd_vmm.c",
                        "FREEBSD_VMM_UART_VA") &&
       !source_contains("kernel/agentos-root-task/src/cc_pd.c",
                        "CC_PD_UART_DBG_VA"),
       "VMM and CC PD sources contain no direct physical UART access");

    ok(!source_contains("kernel/agentos-root-task/linux_vmm_test.system",
                        "phys_addr=\"0x9000000\"") &&
       !source_contains("kernel/agentos-root-task/linux_vmm_test.system",
                        "irq=\"33\""),
       "standalone Linux VMM manifest grants no PL011 frame or IRQ");

    ok(source_contains("kernel/agentos-root-task/src/linux_vmm.c",
                       "fault_mrs[seL4_MsgMaxLength]") &&
       source_contains("kernel/agentos-root-task/src/linux_vmm.c",
                       "seL4_SetMR((int)i, fault_mrs[i])") &&
       source_contains("kernel/agentos-root-task/src/freebsd_vmm.c",
                       "fault_mrs[seL4_MsgMaxLength]") &&
       source_contains("kernel/agentos-root-task/src/freebsd_vmm.c",
                       "seL4_SetMR((int)i, fault_mrs[i])"),
       "VMMs preserve fault MRs across serial_pd diagnostic IPC");

    ok(source_contains("kernel/agentos-root-task/ubuntu-iso-overlay.dts.in",
                       "pl011@9000000") &&
       source_contains("kernel/agentos-root-task/ubuntu-iso-overlay.dts.in",
                       "status = \"disabled\"") &&
       !source_contains("kernel/agentos-root-task/ubuntu-iso-overlay.dts.in",
                        "stdout-path"),
       "Ubuntu DT disables PL011 after address-based earlycon");

    printf("1..%d\n", tests);
    return failures ? 1 : 0;
}
