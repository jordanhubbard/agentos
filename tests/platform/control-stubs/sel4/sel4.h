#pragma once
#include <stdint.h>
typedef uint64_t seL4_Word;
typedef uint64_t seL4_CPtr;
typedef struct { seL4_Word label, caps, extra, length; } seL4_MessageInfo_t;
static inline seL4_MessageInfo_t seL4_MessageInfo_new(
    seL4_Word label, seL4_Word caps, seL4_Word extra, seL4_Word length)
{ return (seL4_MessageInfo_t){label, caps, extra, length}; }
static inline seL4_Word seL4_MessageInfo_get_label(seL4_MessageInfo_t i) { return i.label; }
static inline seL4_Word seL4_MessageInfo_get_length(seL4_MessageInfo_t i) { return i.length; }
static inline seL4_Word seL4_MessageInfo_get_capsUnwrapped(seL4_MessageInfo_t i) { return i.caps; }
static inline seL4_Word seL4_MessageInfo_get_extraCaps(seL4_MessageInfo_t i) { return i.extra; }
seL4_Word seL4_GetMR(int);
void seL4_SetMR(int, seL4_Word);
seL4_MessageInfo_t seL4_Recv(seL4_CPtr, seL4_Word *, seL4_CPtr);
seL4_MessageInfo_t seL4_NBRecv(seL4_CPtr, seL4_Word *, seL4_CPtr);
seL4_MessageInfo_t seL4_Call(seL4_CPtr, seL4_MessageInfo_t);
seL4_MessageInfo_t seL4_ReplyRecv(seL4_CPtr, seL4_MessageInfo_t, seL4_Word *, seL4_CPtr);
void seL4_Send(seL4_CPtr, seL4_MessageInfo_t);
