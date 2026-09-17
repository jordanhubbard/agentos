#pragma once
/* The shared register core uses no seL4 calls or CPU context fields. */
#include <stdint.h>
typedef uintptr_t seL4_Word;
typedef struct seL4_UserContext seL4_UserContext;
/* Declarations only: the x86 adapter does not invoke capability operations.
 * Real architecture builds use the SDK rather than this host-test header. */
typedef uintptr_t seL4_CPtr;
typedef int seL4_Error;
typedef struct { seL4_Word label; } seL4_MessageInfo_t;
static inline seL4_MessageInfo_t seL4_MessageInfo_new(seL4_Word label,
    seL4_Word caps, seL4_Word extra, seL4_Word length)
{
    (void)caps; (void)extra; (void)length;
    return (seL4_MessageInfo_t){label};
}
void seL4_NBSend(seL4_CPtr cap, seL4_MessageInfo_t message);
seL4_MessageInfo_t seL4_Call(seL4_CPtr cap, seL4_MessageInfo_t message);
enum { seL4_NoError = 0 };
seL4_Error seL4_TCB_Suspend(seL4_CPtr cap);
seL4_Error seL4_IRQHandler_Ack(seL4_CPtr cap);
void seL4_Signal(seL4_CPtr cap);
seL4_Word seL4_VMEnter(seL4_Word *badge);
seL4_Word seL4_GetMR(int index);
void seL4_SetMR(int index, seL4_Word value);
