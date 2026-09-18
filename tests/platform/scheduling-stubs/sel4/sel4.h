#ifndef TEST_GUEST_SCHEDULING_SEL4_H
#define TEST_GUEST_SCHEDULING_SEL4_H
#include <stdint.h>
typedef uintptr_t seL4_CPtr;
typedef uintptr_t seL4_Word;
typedef int seL4_Error;
#define seL4_NoError 0
#define seL4_AllRights 15u
seL4_Error seL4_CNode_Copy(seL4_CPtr, seL4_Word, uint8_t,
    seL4_CPtr, seL4_Word, uint8_t, seL4_Word);
seL4_Error seL4_CNode_Delete(seL4_CPtr, seL4_Word, uint8_t);
seL4_Error seL4_SchedControl_ConfigureFlags(seL4_CPtr, seL4_CPtr,
    seL4_Word, seL4_Word, seL4_Word, seL4_Word, seL4_Word);
seL4_Error seL4_TCB_SetSchedParams(seL4_CPtr, seL4_CPtr,
    seL4_Word, seL4_Word, seL4_CPtr, seL4_CPtr);
#endif
