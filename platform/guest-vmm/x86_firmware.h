#ifndef AOS_X86_FIRMWARE_H
#define AOS_X86_FIRMWARE_H
#include "sel4_boot.h"
#include <platform/x86_vmenter.h>
/* First reset guest return is captured before firmware initialization. */
void aos_x86_firmware_run(seL4_CPtr endpoint, aos_x86_vmenter_return_t returned);
#endif
