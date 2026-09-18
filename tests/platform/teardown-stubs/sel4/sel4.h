#pragma once
#include <stdint.h>
typedef uint64_t seL4_CPtr;
typedef uint64_t seL4_Word;
typedef int seL4_Error;
#define seL4_NoError 0
seL4_Error seL4_CNode_Revoke(seL4_CPtr, seL4_Word, uint8_t);
