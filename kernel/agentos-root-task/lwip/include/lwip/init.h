/*
 * lwip/init.h — lwIP initialisation shim
 */
#ifndef LWIP_INIT_H
#define LWIP_INIT_H

/* Initialise the lwIP stack (NO_SYS=1: synchronous, no threads) */
void lwip_init(void);

#endif /* LWIP_INIT_H */
