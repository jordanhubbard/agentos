#ifndef TEST_X86_OBJECTS_SEL4_H
#define TEST_X86_OBJECTS_SEL4_H
#include <stdint.h>
typedef uintptr_t seL4_CPtr;
typedef uintptr_t seL4_Word;
typedef int seL4_Error;
typedef seL4_Word seL4_CapRights_t;
#define seL4_AllRights 3u
#define seL4_X86_Default_VMAttributes 0u
static inline seL4_CapRights_t seL4_CapRights_new(unsigned grant_reply,
    unsigned grant, unsigned read, unsigned write)
{ (void)grant_reply; (void)grant; return read | (write << 1u); }
#define seL4_NoError 0
#define seL4_InvalidArgument 2
#define seL4_UntypedObject 1u
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
seL4_Error seL4_CNode_Copy(seL4_CPtr, seL4_Word, uint8_t,
    seL4_CPtr, seL4_Word, uint8_t, seL4_CapRights_t);
seL4_Error seL4_X86_Page_Map(seL4_CPtr, seL4_CPtr, seL4_Word, seL4_CapRights_t, seL4_Word);
seL4_Error seL4_X86_Page_MapEPT(seL4_CPtr, seL4_CPtr, seL4_Word, seL4_CapRights_t, seL4_Word);
seL4_Error seL4_X86_Page_Unmap(seL4_CPtr);
seL4_Error seL4_TCB_SetEPTRoot(seL4_CPtr, seL4_CPtr);
seL4_Error seL4_X86_VCPU_SetTCB(seL4_CPtr, seL4_CPtr);
seL4_Error seL4_CNode_Revoke(seL4_CPtr,seL4_Word,uint8_t);
#endif
