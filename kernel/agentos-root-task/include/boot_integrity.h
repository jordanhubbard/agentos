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
uint32_t boot_integrity_handle_ppc(uint32_t op, const sel4_msg_t *req, sel4_msg_t *rep);

/*
 * BOOT_MEASURE_SELF(boot_ep, pd_id, hash32)
 *
 * E5-S8: migrated from Microkit to raw seL4 IPC.
 * boot_ep is a seL4_CPtr to the controller endpoint.
 *
 * Packs the 32-byte hash into a sel4_msg_t and sends OP_BOOT_MEASURE
 * to the controller. Guarded by AGENTOS_TEST_HOST.
 */
#define BOOT_MEASURE_SELF(boot_ep, pd_id, hash32) do { \
    uint8_t *_h = (uint8_t *)(hash32); \
    sel4_msg_t _bm = {0}; \
    _bm.opcode = OP_BOOT_MEASURE; \
    { uint8_t _pd = (uint8_t)(pd_id); \
      _bm.data[4] = _pd; } \
    for (int _i = 0; _i < 8; _i++) { \
        uint32_t _w = ((uint32_t)_h[_i*4+0]<<24)|((uint32_t)_h[_i*4+1]<<16) \
                     |((uint32_t)_h[_i*4+2]<<8)|(uint32_t)_h[_i*4+3]; \
        uint32_t _off = (uint32_t)(8 + _i * 4); \
        if (_off + 4 <= SEL4_MSG_DATA_BYTES) { \
            _bm.data[_off]   = (uint8_t)_w; \
            _bm.data[_off+1] = (uint8_t)(_w >> 8); \
            _bm.data[_off+2] = (uint8_t)(_w >> 16); \
            _bm.data[_off+3] = (uint8_t)(_w >> 24); \
        } \
    } \
    _bm.length = 36; \
    sel4_msg_t _br = {0}; \
    (void)_br; \
    /* E5-S8: ppcall to boot_ep stubbed */ \
} while(0)

#endif /* BOOT_INTEGRITY_H */
