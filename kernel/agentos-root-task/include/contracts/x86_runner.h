#ifndef AOS_CONTRACT_X86_RUNNER_H
#define AOS_CONTRACT_X86_RUNNER_H
#include <stdint.h>

/* Private coordinator/executor IPC, x86-64 native words only. Root grants the
 * endpoint solely to the owning VMM. The executor has its own native VSpace,
 * IPC buffer and TCB; the kernel binds that TCB to one already admitted VCPU.
 * No guest pointer, device capability or caller-selected VCPU crosses IPC.
 * One synchronous invocation executes one VMEnter. Sequence numbers never
 * wrap and a duplicate request must not execute the guest a second time. */
#define AOS_X86_RUNNER_VERSION UINT64_C(1)
#define AOS_X86_RUNNER_OWNER_BADGE UINT64_C(1)
#define AOS_X86_RUNNER_ENDPOINT_CAP 474u
#define AOS_X86_RUNNER_ENTER 1u
#define AOS_X86_RUNNER_RETURN 2u
#define AOS_X86_RUNNER_REJECT 3u
#define AOS_X86_RUNNER_ENTRY_WORDS 3u
#define AOS_X86_RUNNER_FAULT_WORDS 25u
#define AOS_X86_RUNNER_NOTIFICATION_WORDS 3u
#define AOS_X86_RUNNER_REQUEST_WORDS 5u
#define AOS_X86_RUNNER_REPLY_HEADER_WORDS 5u
#define AOS_X86_RUNNER_REPLY_MAX_WORDS 30u
#define AOS_X86_RUNNER_NOTIFICATION 0u
#define AOS_X86_RUNNER_FAULT 1u

typedef struct {
    uint64_t version, sequence;
    uint64_t entry[AOS_X86_RUNNER_ENTRY_WORDS];
} aos_x86_runner_request_t;

typedef struct {
    uint64_t version, sequence, result, badge, count;
    uint64_t words[AOS_X86_RUNNER_FAULT_WORDS];
} aos_x86_runner_reply_t;

_Static_assert(sizeof(aos_x86_runner_request_t)==8u*AOS_X86_RUNNER_REQUEST_WORDS,
               "runner request word layout");
_Static_assert(sizeof(aos_x86_runner_reply_t)==8u*AOS_X86_RUNNER_REPLY_MAX_WORDS,
               "runner reply word layout");
#endif
