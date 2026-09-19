#ifndef AOS_X86_FIRMWARE_H
#define AOS_X86_FIRMWARE_H
#include "sel4_boot.h"
#include <platform/x86_vmenter.h>
/* Requires a stopped, fresh VCPU. Never enters the guest. Output is published
 * only on success; any failure requires partial-object revocation before retry.
 * failed_field identifies the VMCS field, or zero for a register-write failure. */
seL4_Error aos_x86_firmware_reset(aos_x86_vmenter_entry_t *entry,
                                  seL4_Word *failed_field);
/* Explicit admitted capability; no global selection or native VM entry. */
seL4_Error aos_x86_firmware_reset_cpu(seL4_CPtr vcpu,
    aos_x86_vmenter_entry_t *entry, seL4_Word *failed_field);
/* Fresh, stopped VCPU only. Configure the SIPI page in real mode. The caller
 * must recreate execution state for INIT before using this after a prior run.
 * Invalid vector/cap/output pointers are rejected before configuration.
 * Partial native failures leave entry unchanged and require object cleanup. */
seL4_Error aos_x86_firmware_startup_cpu(seL4_CPtr vcpu, unsigned vector,
    aos_x86_vmenter_entry_t *entry, seL4_Word *failed_field);
/* Devices and lifecycle admission are initialized before the first VM entry. */
_Noreturn void aos_x86_firmware_run(seL4_CPtr endpoint, aos_x86_vmenter_entry_t entry);
#endif
