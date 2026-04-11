/*
 * lwip/pbuf.h — packet buffer shim for agentOS lwIP-compatible layer
 */
#ifndef LWIP_PBUF_H
#define LWIP_PBUF_H

#include "lwip/arch.h"
#include "lwip/err.h"
#include <stdint.h>
#include <stddef.h>

typedef enum {
    PBUF_TRANSPORT = 0,
    PBUF_IP        = 1,
    PBUF_LINK      = 2,
    PBUF_RAW_TX    = 3,
    PBUF_RAW       = 4,
} pbuf_layer;

typedef enum {
    PBUF_RAM   = 0,
    PBUF_ROM   = 1,
    PBUF_REF   = 2,
    PBUF_POOL  = 3,
} pbuf_type;

struct pbuf {
    struct pbuf *next;
    void        *payload;
    u16_t        tot_len;
    u16_t        len;
    u8_t         type_internal;
    u8_t         flags;
    u16_t        ref;
    u16_t        if_idx;
};

struct pbuf *pbuf_alloc(pbuf_layer layer, u16_t length, pbuf_type type);
void         pbuf_free(struct pbuf *p);
u16_t        pbuf_copy_partial(const struct pbuf *buf, void *dataptr,
                                u16_t len, u16_t offset);
err_t        pbuf_take(struct pbuf *buf, const void *dataptr, u16_t len);

#endif /* LWIP_PBUF_H */
