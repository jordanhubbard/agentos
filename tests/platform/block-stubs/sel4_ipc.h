#pragma once
/* Mock only the IPC boundary; retain the production message ABI. */
#include <sel4/sel4.h>
#include "sel4_msg_types.h"
void sel4_call(seL4_CPtr, const sel4_msg_t *, sel4_msg_t *);
