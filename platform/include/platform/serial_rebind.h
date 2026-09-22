#ifndef AOS_PLATFORM_SERIAL_REBIND_H
#define AOS_PLATFORM_SERIAL_REBIND_H
#include <stdbool.h>
#include <stdint.h>
/* Call after detach and pool revocation, with the receive slot empty. On
 * failure keep execution stopped, detach any committed service generation,
 * then revoke the pool before retry. No guest byte uses control IPC. */
bool aos_serial_virt_rebind(uint32_t client, uint32_t generation);
#endif
