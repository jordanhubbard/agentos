#ifndef AOS_PLATFORM_X86_MEMORY_REBUILD_H
#define AOS_PLATFORM_X86_MEMORY_REBUILD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Requires completed terminal teardown, rebuilt EPT and empty frame slots.
 * image is a retained native read-only firmware blob, outside guest mappings.
 * Translation remains disabled. On failure revoke all memory pools before
 * retrying; never execute a partially restored guest. */
bool aos_x86_guest_memory_rebuild(const uint8_t *image, size_t image_size,
                                  size_t ram_size);
#endif
