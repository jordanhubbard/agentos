/*
 * lwip/timeouts.h — NO_SYS=1 timeout polling shim
 */
#ifndef LWIP_TIMEOUTS_H
#define LWIP_TIMEOUTS_H

#include "lwip/arch.h"

/* Called periodically to drive lwIP internal timers (NO_SYS=1 mode) */
void sys_check_timeouts(void);

/* Returns monotonic time in milliseconds (provided by lwip_sys.c) */
u32_t sys_now(void);

#endif /* LWIP_TIMEOUTS_H */
