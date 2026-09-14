#ifndef AOS_LOG_ISOLATION_PROBE_H
#define AOS_LOG_ISOLATION_PROBE_H
#include <platform/log_ring.h>
#define AOS_LOG_PROOF "native log proof: first fragment + second\n"
#ifdef AGENTOS_LOG_ISOLATION_PROBE
#if AGENTOS_LOG_ISOLATION_PROBE < 1 || AGENTOS_LOG_ISOLATION_PROBE > 3
#error "log isolation probe must be 1..3"
#endif
#define AOS_LOG_PROBE_BADGE (0xa0560000u + AGENTOS_LOG_ISOLATION_PROBE)
#define AOS_LOG_PROBE_WRITE (AGENTOS_LOG_ISOLATION_PROBE != 1)
#define AOS_LOG_PROBE_ADDRESS (AGENTOS_LOG_ISOLATION_PROBE == 3 ? AOS_LOG_CONFIG_VA : AOS_LOG_SERVER_VA)
#define AOS_LOG_PROBE_MESSAGE "[rt] log isolation: expected client data fault verified\n"
#endif
#endif
