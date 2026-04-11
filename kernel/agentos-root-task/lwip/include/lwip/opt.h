/*
 * lwip/opt.h — redirects to lwipopts.h (user configuration)
 */
#ifndef LWIP_OPT_H
#define LWIP_OPT_H

#include "lwipopts.h"

/* Derived defaults for anything not set in lwipopts.h */
#ifndef LWIP_IPV4
#define LWIP_IPV4 1
#endif
#ifndef LWIP_IPV6
#define LWIP_IPV6 0
#endif

#endif /* LWIP_OPT_H */
