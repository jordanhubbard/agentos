#ifndef AOS_X86_FIRMWARE_H
#define AOS_X86_FIRMWARE_H
#include "sel4_boot.h"
/* Called with the first reset guest exit still present in the message registers. */
void aos_x86_firmware_run(seL4_CPtr endpoint, seL4_Word result);
#endif
