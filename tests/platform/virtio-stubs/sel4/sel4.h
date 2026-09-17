#pragma once
/* The shared register core uses no seL4 calls or CPU context fields. */
#include <stdint.h>
typedef uintptr_t seL4_Word;
typedef struct seL4_UserContext seL4_UserContext;
