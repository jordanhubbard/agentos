/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef AGENTOS_INPUT_VIRT_CONTRACT_H
#define AGENTOS_INPUT_VIRT_CONTRACT_H
#include <stdint.h>
/* Control only. Event delivery remains in platform/input.h shared queues.
 * REBIND transfers one owning VMM's empty private queue pool; success returns
 * one fresh frame cap and the exact new generation. Root-minted endpoint
 * badges authorize client selection. No request transfers peer authority. */
#define INPUT_VIRT_OP_REBIND 1u
/* Same request/reply payload, no capabilities. Retire a reconstruction
 * attachment even when the VMM could not map its returned queue frame. */
#define INPUT_VIRT_OP_RETIRE 2u
#define INPUT_VIRT_REBIND_VERSION 1u
typedef struct { uint32_t version, client, generation; } input_virt_rebind_req_t;
typedef struct { uint32_t status, version, generation; } input_virt_rebind_reply_t;
_Static_assert(sizeof(input_virt_rebind_req_t)==12, "input rebind request ABI");
_Static_assert(sizeof(input_virt_rebind_reply_t)==12, "input rebind reply ABI");
#endif
