/* Private x86 firmware RAM/ROM allocation authority, not host device caps. */
#ifndef AGENTOS_X86_GUEST_MEMORY_CAPS_H
#define AGENTOS_X86_GUEST_MEMORY_CAPS_H

#include "contracts/guest_ram_caps.h"

/* Reuse the architecture-neutral RAM ranges, with separate ROM pools so
 * the maximum RAM reservation cannot spill into frame/alias slots. */
#define AOS_X86_GUEST_ROM_POOL_BASE 496u
#define AOS_X86_GUEST_ROM_FRAMES 2u
#define AOS_X86_GUEST_ROM_FRAME_BASE 502u
#define AOS_X86_GUEST_ROM_ALIAS_BASE 504u

/* Each RAM or ROM frame and all its aliases descend from one private
 * non-device 2 MiB child untyped. Root moves its sole pool cap after mapping.
 * ROM is read-only in both EPT and the VMM. Revocation requires quiesced I/O
 * and no further VMEnter; the VMM's code, stack and thread are separate. */

#endif
