/* Linkable wrappers for seL4's static-inline IPC-buffer accessors.
 * pd_entry.c must initialize __sel4_ipc_buffer before Rust executes.
 * These wrappers confer no endpoint or device authority. */
#include <sel4/sel4.h>

_Static_assert(sizeof(seL4_Word) == 8, "Rust PD bridge requires 64-bit seL4");
_Static_assert(seL4_MsgMaxLength == 120, "Update Rust MESSAGE_REGISTERS for this SDK");

seL4_Word agentos_pd_get_mr(int index)
{
    if (index < 0 || index >= seL4_MsgMaxLength) return 0;
    return seL4_GetMR(index);
}

void agentos_pd_set_mr(int index, seL4_Word value)
{
    if (index < 0 || index >= seL4_MsgMaxLength) return;
    seL4_SetMR(index, value);
}
