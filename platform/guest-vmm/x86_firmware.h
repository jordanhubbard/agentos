#ifndef AOS_X86_FIRMWARE_H
#define AOS_X86_FIRMWARE_H
#include "sel4_boot.h"
#include <platform/x86_vmenter.h>
/* Requires a stopped, fresh VCPU. Never enters the guest. Output is published
 * only on success; any failure requires partial-object revocation before retry.
 * failed_field identifies the VMCS field, or zero for a register-write failure. */
seL4_Error aos_x86_firmware_reset(aos_x86_vmenter_entry_t *entry,
                                  seL4_Word *failed_field);
/* Devices and lifecycle admission are initialized before the first VM entry. */
_Noreturn void aos_x86_firmware_run(seL4_CPtr endpoint, aos_x86_vmenter_entry_t entry);
#endif
