/* Test-only native-client faults. Production builds define no probe mode. */
#ifndef AOS_NATIVE_NET_ISOLATION_PROBE_H
#define AOS_NATIVE_NET_ISOLATION_PROBE_H
#include <platform/net_host_layout.h>
#ifdef AGENTOS_NATIVE_NET_ISOLATION_PROBE
#if AGENTOS_NATIVE_NET_ISOLATION_PROBE < 1 || AGENTOS_NATIVE_NET_ISOLATION_PROBE > 10
#error "native network isolation probe must be 1..10"
#endif
#define AOS_NATIVE_NET_PROBE_RESOURCE ((AGENTOS_NATIVE_NET_ISOLATION_PROBE - 1u) / 2u)
#define AOS_NATIVE_NET_PROBE_WRITE ((AGENTOS_NATIVE_NET_ISOLATION_PROBE % 2u) == 0u)
#define AOS_NATIVE_NET_PROBE_BADGE (0xa0530000u + AGENTOS_NATIVE_NET_ISOLATION_PROBE)
/* Read and write each guest's queue page, driver transfer page, NIC MMIO and
 * driver DMA window. Addresses come from the production mapping contracts. */
#define AOS_NATIVE_NET_PROBE_ADDRESS \
    (AOS_NATIVE_NET_PROBE_RESOURCE == 0u ? AOS_NET_SHMEM_VA : \
     AOS_NATIVE_NET_PROBE_RESOURCE == 1u ? AOS_NET_SHMEM_VA + AOS_NET_CLIENT_STRIDE : \
     AOS_NATIVE_NET_PROBE_RESOURCE == 2u ? AOS_NET_SHMEM_VA + AOS_NET_DRIVER_SLOT_BASE : \
     AOS_NATIVE_NET_PROBE_RESOURCE == 3u ? AGENTOS_HOST_NET_MMIO_VA : AGENTOS_NET_HOST_DMA_VA)
#define AOS_NATIVE_NET_PROBE_MESSAGE "[rt] native network isolation: expected client data fault verified\n"
#endif
#endif
