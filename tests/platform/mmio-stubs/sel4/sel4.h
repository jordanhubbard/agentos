#pragma once
#include <stdint.h>
typedef uint64_t seL4_Word;
typedef uint64_t seL4_CPtr;
void seL4_Signal(seL4_CPtr cap);
void seL4_Wait(seL4_CPtr cap,seL4_Word *badge);
typedef uint64_t seL4_MessageInfo_t;
typedef struct { uint64_t pc, x0; } seL4_UserContext;
