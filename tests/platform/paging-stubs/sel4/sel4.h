#ifndef TEST_GUEST_PAGING_SEL4_H
#define TEST_GUEST_PAGING_SEL4_H
#include <stdint.h>
typedef uintptr_t seL4_CPtr;
typedef uintptr_t seL4_Word;
typedef int seL4_Error;
#define seL4_NoError 0
#define seL4_FailedLookup 2
#define seL4_AllRights 15u
#define seL4_ARM_Default_VMAttributes 3u
#define seL4_ARM_VSpaceObject 1u
#define seL4_ARM_PageTableObject 2u
#define seL4_VSpaceBits 13u
#define seL4_PageTableBits 12u
seL4_Error seL4_CNode_Revoke(seL4_CPtr, seL4_Word, uint8_t);
seL4_Error seL4_Untyped_Retype(seL4_CPtr, seL4_Word, seL4_Word,
    seL4_CPtr, seL4_Word, seL4_Word, seL4_Word, seL4_Word);
seL4_Error seL4_ARM_ASIDPool_Assign(seL4_CPtr, seL4_CPtr);
seL4_Error seL4_ARM_Page_Map(seL4_CPtr, seL4_CPtr, seL4_Word, seL4_Word, seL4_Word);
seL4_Error seL4_ARM_PageTable_Map(seL4_CPtr, seL4_CPtr, seL4_Word, seL4_Word);
#endif
