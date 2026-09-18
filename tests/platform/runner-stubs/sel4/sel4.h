#pragma once
#include <stdint.h>
typedef uintptr_t seL4_Word;
typedef uintptr_t seL4_CPtr;
typedef struct { seL4_Word label,caps,extra,length; } seL4_MessageInfo_t;
static inline seL4_MessageInfo_t seL4_MessageInfo_new(seL4_Word label,
    seL4_Word caps,seL4_Word extra,seL4_Word length)
{ return (seL4_MessageInfo_t){label,caps,extra,length}; }
static inline seL4_Word seL4_MessageInfo_get_label(seL4_MessageInfo_t m) { return m.label; }
static inline seL4_Word seL4_MessageInfo_get_capsUnwrapped(seL4_MessageInfo_t m) { return m.caps; }
static inline seL4_Word seL4_MessageInfo_get_extraCaps(seL4_MessageInfo_t m) { return m.extra; }
static inline seL4_Word seL4_MessageInfo_get_length(seL4_MessageInfo_t m) { return m.length; }
seL4_Word seL4_GetMR(int);
void seL4_SetMR(int,seL4_Word);
seL4_Word seL4_VMEnter(seL4_Word *);
#ifdef CONFIG_KERNEL_MCS
seL4_MessageInfo_t seL4_Recv(seL4_CPtr,seL4_Word *,seL4_CPtr);
void seL4_Send(seL4_CPtr,seL4_MessageInfo_t);
#else
seL4_MessageInfo_t seL4_Recv(seL4_CPtr,seL4_Word *);
void seL4_Reply(seL4_MessageInfo_t);
#endif
