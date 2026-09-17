#pragma once
#include <stdint.h>
typedef uint64_t seL4_Word;
typedef uint64_t seL4_CPtr;
typedef uint64_t seL4_MessageInfo_t;
typedef struct { uint64_t pc, x0; } seL4_UserContext;
