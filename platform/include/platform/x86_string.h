#ifndef AOS_X86_STRING_H
#define AOS_X86_STRING_H
#include "platform/x86_memory.h"
#include "platform/x86_config.h"

#define AOS_X86_STRING_BATCH 256u
/* One bounded chunk of long-mode REP INSB from fw_cfg. All destinations are
 * checked before advancing device state or touching RAM. The caller resumes
 * the same instruction when count remains nonzero. False changes nothing. */
bool aos_x86_fw_insb(const aos_x86_memory_t *memory, uint8_t *writable_ram,
                      uint64_t cr3, aos_x86_config_t *config,
                      uint64_t *address, uint64_t *count, bool backwards);
#endif
