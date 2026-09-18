#ifndef AOS_PLATFORM_X86_RUNNER_H
#define AOS_PLATFORM_X86_RUNNER_H
#include <stdbool.h>
#include <stddef.h>
#include <contracts/x86_runner.h>

typedef struct {
    uint64_t next_sequence;
    bool failed, active;
} aos_x86_runner_t;

/* Kernel-return snapshot captured before any reply IPC overwrites MRs. */
typedef struct {
    uint64_t result, badge;
    uint64_t words[AOS_X86_RUNNER_FAULT_WORDS];
} aos_x86_runner_exit_t;
typedef bool (*aos_x86_runner_enter_fn)(void *, const uint64_t *, aos_x86_runner_exit_t *);

/* Root/owner initialization only; do not reset on rejected or duplicate IPC. */
void aos_x86_runner_init(aos_x86_runner_t *);
/* Serialized server step. Invalid/replayed requests leave state unchanged.
 * Reserve the sequence before VMEnter; a failed/invalid return permanently
 * latches failure so an ambiguous execution can never be retried. Outputs
 * remain unchanged on failure. Notification replies expose only three MRs. */
bool aos_x86_runner_step(aos_x86_runner_t *, unsigned label,
                         const aos_x86_runner_request_t *, size_t request_words,
                         aos_x86_runner_enter_fn, void *, aos_x86_runner_reply_t *);
/* Client validation binds a response to one outstanding request. */
bool aos_x86_runner_reply_valid(const aos_x86_runner_reply_t *, size_t words,
                                uint64_t expected_sequence);
#endif
