/*
 * lwip/arch.h — architecture-specific types for agentOS bare-metal
 */
#ifndef LWIP_ARCH_H
#define LWIP_ARCH_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uintptr_t mem_ptr_t;

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

#define LWIP_PLATFORM_ASSERT(x) do { (void)(x); } while(0)
#define LWIP_PLATFORM_DIAG(x)   do { } while(0)

#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

#endif /* LWIP_ARCH_H */
