/*
 * lwip/err.h — lwIP error type shim for agentOS NO_SYS=1 build
 * Provides a minimal err_t and ERR_* constants matching lwIP 2.2.0 API.
 */
#ifndef LWIP_ERR_H
#define LWIP_ERR_H

#include <stdint.h>

typedef int8_t err_t;

#define ERR_OK          ((err_t)  0)
#define ERR_MEM         ((err_t) -1)
#define ERR_BUF         ((err_t) -2)
#define ERR_TIMEOUT     ((err_t) -3)
#define ERR_RTE         ((err_t) -4)
#define ERR_INPROGRESS  ((err_t) -5)
#define ERR_VAL         ((err_t) -6)
#define ERR_WOULDBLOCK  ((err_t) -7)
#define ERR_USE         ((err_t) -8)
#define ERR_ALREADY     ((err_t) -9)
#define ERR_ISCONN      ((err_t) -10)
#define ERR_CONN        ((err_t) -11)
#define ERR_IF          ((err_t) -12)
#define ERR_ABRT        ((err_t) -13)
#define ERR_RST         ((err_t) -14)
#define ERR_CLSD        ((err_t) -15)
#define ERR_ARG         ((err_t) -16)

#endif /* LWIP_ERR_H */
