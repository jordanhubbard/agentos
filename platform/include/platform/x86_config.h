#ifndef AOS_X86_CONFIG_H
#define AOS_X86_CONFIG_H
#include <stdbool.h>
#include <stdint.h>
#include "platform/x86_rtc.h"
#include "platform/x86_acpi.h"

#define AOS_X86_BOOT_BLOB_LIMIT (64u*1024u*1024u)
#define AOS_X86_BOOT_CMDLINE_LIMIT 4096u
/* Immutable VMM-owned sources, not guest pointers. Keep storage alive until
 * guest teardown. The kernel is a complete EFI image; setup/legacy addresses
 * are not supplied. OVMF validates and loads the image through its EFI path. */
typedef struct {
    const uint8_t *kernel, *initrd, *cmdline;
    uint32_t kernel_size, initrd_size, cmdline_size;
} aos_x86_boot_blobs_t;

typedef struct {
    uint32_t pci_address, ram_bytes, fw_offset;
    uint16_t fw_selector;
    uint8_t host[256], pm[256], cmos_index;
    uint8_t cmos_shutdown; /* private cold/warm CPU-start marker, no S3 state */
    uint32_t pci_reads, timer_reads, fw_reads;
    uint64_t fw_dma_address;
    uint32_t cpu_selector;
    uint8_t cpu_command;
    uint8_t pit_disable_remaining;
    uint8_t reset_control;
    /* Guest-private reset latch. The coordinator must stop re-entry and
     * drain/reconstruct this guest before consuming another exit. */
    bool reset_requested;
    aos_x86_rtc_t rtc;
    /* Private ACPI PM1: polled TMR_STS/W1C, no enabled SCI sources; control
     * retains SCI_EN, BM_RLD and SLP_TYP but rejects sleep/SMI requests. */
    uint16_t pm_status, pm_control;
    uint64_t pm_last_ticks;
    aos_x86_boot_blobs_t boot;
    const aos_x86_acpi_bundle_t *acpi;
    uint32_t boot_reads[3]; /* bytes consumed in kernel/initrd/cmdline, saturating */
} aos_x86_config_t;

/* One state per guest; timer_ticks is supplied by the VMM's virtual clock.
 * False rejects unsupported ports/widths without mutating state or value. */
bool aos_x86_config_init(aos_x86_config_t *s, uint32_t ram_bytes);
/* Bind once, before the first fw_cfg read. Kernel required; optional initrd
 * and printable ASCII command line (size includes final NUL). Rejects all
 * invalid descriptors without changing state. This does not prove EFI entry. */
bool aos_x86_config_boot(aos_x86_config_t *s, const aos_x86_boot_blobs_t *boot);
/* Bind a bundle produced by an aos_x86_acpi_bundle initializer before fw_cfg reads.
 * Storage remains immutable and alive until guest teardown. */
bool aos_x86_config_acpi(aos_x86_config_t *s, const aos_x86_acpi_bundle_t *acpi);
bool aos_x86_config_io(aos_x86_config_t *s, uint16_t port, unsigned width,
                       bool write, uint32_t *value, uint64_t timer_ticks);
/* Execute one bounded fw_cfg DMA read/skip after the caller has validated the
 * guest descriptor and destination. Control uses the standard big-endian
 * descriptor value after decoding. All configuration state is atomic on
 * rejection; fw_cfg items remain read-only. */
bool aos_x86_config_dma(aos_x86_config_t *s, uint32_t control,
                        uint8_t *destination, uint32_t length);
#endif
