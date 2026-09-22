#ifndef TEST_EXECUTION_SEL4_H
#define TEST_EXECUTION_SEL4_H
#include <stdint.h>
typedef uintptr_t seL4_CPtr;
typedef uintptr_t seL4_Word;
typedef int seL4_Error;
#define seL4_NoError 0
#define seL4_AllRights 15u
#define seL4_TCBObject 1u
#define seL4_ARM_VCPUObject 2u
#define seL4_SchedContextObject 3u
#define seL4_ARM_SmallPageObject 4u
#define seL4_TCBBits 12u
#define seL4_VCPUBits 12u
#define seL4_PageBits 12u
#define seL4_MinSchedContextBits 7u
#define seL4_WordBits 64u
seL4_Error seL4_CNode_Revoke(seL4_CPtr, seL4_Word, uint8_t);
seL4_Error seL4_Untyped_Retype(seL4_CPtr, seL4_Word, seL4_Word, seL4_CPtr, seL4_Word, seL4_Word, seL4_Word, seL4_Word);
seL4_Error seL4_CNode_Copy(seL4_CPtr, seL4_Word, uint8_t, seL4_CPtr, seL4_Word, uint8_t, seL4_Word);
seL4_Error seL4_TCB_Configure(seL4_CPtr, seL4_CPtr, seL4_Word, seL4_CPtr, seL4_Word, seL4_Word, seL4_CPtr);
seL4_Error seL4_ARM_VCPU_SetTCB(seL4_CPtr, seL4_CPtr);
seL4_Error seL4_ARM_Page_Unmap(seL4_CPtr);
#endif
