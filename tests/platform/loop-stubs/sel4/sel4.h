#pragma once
#include <stdint.h>
typedef uint64_t seL4_Word;
typedef uint64_t seL4_CPtr;
typedef struct { seL4_Word label; } seL4_MessageInfo_t;
#define seL4_Fault_NullFault 0u
static inline seL4_Word seL4_MessageInfo_get_label(seL4_MessageInfo_t info)
{
    return info.label;
}
seL4_MessageInfo_t seL4_Recv(seL4_CPtr, seL4_Word *, seL4_CPtr);
void seL4_Send(seL4_CPtr, seL4_MessageInfo_t);
