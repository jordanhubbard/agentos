/*
 * boot_integrity.h — agentOS Boot Integrity Measurement API
 *
 * Include in monitor.c / controller.c to enable boot attestation.
 * Call boot_integrity_init() from init().
 * Forward OP_BOOT_* PPC calls to boot_integrity_handle_ppc().
 */

#ifndef BOOT_INTEGRITY_H
#define BOOT_INTEGRITY_H

#include "agentos.h"
#include <stdint.h>
#include <stdbool.h>

#define BOOT_INTEGRITY_HASH_LEN 32

/* Opcode constants (also in agentos.h) */
#define OP_BOOT_MEASURE  0xB0
#define OP_BOOT_SEAL     0xB1
#define OP_BOOT_QUOTE    0xB2
#define OP_BOOT_VERIFY   0xB3
#define OP_BOOT_RESET    0xB4

void boot_integrity_init(void);
uint32_t boot_integrity_measure(uint32_t pd_id,
                                 const uint8_t measurement[BOOT_INTEGRITY_HASH_LEN]);
microkit_msginfo boot_integrity_handle_ppc(uint32_t op, microkit_msginfo msginfo);

/*
 * BOOT_MEASURE_SELF(boot_ch, pd_id, code_region_ptr, code_region_len)
 *
 * Convenience macro: compute SHA-256 of the code region and send
 * OP_BOOT_MEASURE to the controller (boot_ch channel).
 *
 * Call from each PD's init() to contribute to the measurement chain.
 *
 * For Microkit PDs without OpenSSL, the hash is computed over the
 * program image range accessible via the code_region map.
 */
#define BOOT_MEASURE_SELF(boot_ch, pd_id, hash32) do { \
    uint8_t *_h = (uint8_t *)(hash32); \
    microkit_mr_set(1, (pd_id)); \
    /* Pack 32-byte hash as 8 u32 MRs (big-endian) */ \
    for (int _i = 0; _i < 8; _i++) { \
        uint32_t _w = ((uint32_t)_h[_i*4+0]<<24)|((uint32_t)_h[_i*4+1]<<16) \
                     |((uint32_t)_h[_i*4+2]<<8)|(uint32_t)_h[_i*4+3]; \
        microkit_mr_set(2 + _i, _w); \
    } \
    PPCALL_DONATE((boot_ch), \
        microkit_msginfo_new(OP_BOOT_MEASURE, 9), \
        PRIO_INIT_AGENT, PRIO_CONTROLLER); \
} while(0)

#endif /* BOOT_INTEGRITY_H */
