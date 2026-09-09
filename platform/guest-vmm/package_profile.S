/* Embed one compiler-validated guest profile in a VMM PD. */

#ifndef GUEST_PROFILE_PATH
#error "GUEST_PROFILE_PATH must name a compiled guest profile manifest"
#endif

.section .guest_profile, "a", @progbits
.balign 8
.global _guest_profile, _guest_profile_end
_guest_profile:
.incbin GUEST_PROFILE_PATH
_guest_profile_end:

