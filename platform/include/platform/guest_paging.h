#ifndef AOS_PLATFORM_GUEST_PAGING_H
#define AOS_PLATFORM_GUEST_PAGING_H
#include <stdbool.h>
#include <stdint.h>

/* ARM only. Stop all vCPUs and drain device references before release.
 * Rebuild requires successful release. On any rebuild/map failure, release
 * partial paging objects before retrying; never boot a partial VSpace.
 * The private ASID pool is retained, not the global ASID controller. */
bool aos_vmm_guest_paging_release(void);
bool aos_vmm_guest_paging_rebuild(void);
/* Map only into this VMM's guest VSpace, allocating bounded private tables
 * on FailedLookup. Frame identity/size is supplied by the trusted caller. */
bool aos_vmm_guest_page_map(uintptr_t frame, uintptr_t guest_address);
#endif
