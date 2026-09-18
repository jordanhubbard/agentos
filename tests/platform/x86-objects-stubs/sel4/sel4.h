#ifndef TEST_X86_OBJECTS_SEL4_H
#define TEST_X86_OBJECTS_SEL4_H
#include <stdint.h>
typedef uintptr_t seL4_CPtr;
typedef uintptr_t seL4_Word;
typedef int seL4_Error;
#define seL4_NoError 0
#define seL4_X86_VCPUBits 14u
#define seL4_X86_EPTPML4Bits 12u
#define seL4_X86_EPTPDPTBits 12u
#define seL4_X86_EPTPDBits 12u
#define seL4_X86_VCPUObject 10u
#define seL4_X86_EPTPML4Object 11u
#define seL4_X86_EPTPDPTObject 12u
#define seL4_X86_EPTPDObject 13u
#define seL4_X86_LargePageObject 14u
#define seL4_X86_EPT_Default_VMAttributes 6u
seL4_Error seL4_Untyped_Retype(seL4_CPtr, seL4_Word, seL4_Word,
    seL4_CPtr, seL4_Word, seL4_Word, seL4_Word, seL4_Word);
seL4_Error seL4_X86_ASIDPool_Assign(seL4_CPtr, seL4_CPtr);
seL4_Error seL4_X86_EPTPDPT_Map(seL4_CPtr, seL4_CPtr, seL4_Word, seL4_Word);
seL4_Error seL4_X86_EPTPD_Map(seL4_CPtr, seL4_CPtr, seL4_Word, seL4_Word);
#endif
