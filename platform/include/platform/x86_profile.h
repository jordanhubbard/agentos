#ifndef AOS_X86_PROFILE_H
#define AOS_X86_PROFILE_H
#include <platform/guest_profile.h>
#include <platform/x86_config.h>

/* VMM-owned manifest and immutable blobs only; no guest pointers. Image
 * relocation remains firmware-owned. Reject unsupported profile policy. */
bool aos_x86_profile_bind(const void *manifest, size_t manifest_bytes,
                          const aos_x86_boot_blobs_t *boot,
                          uint64_t ram_bytes, uint64_t ram_hva);
#endif
