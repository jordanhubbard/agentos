#ifndef AOS_X86_RUNNER_CLIENT_H
#define AOS_X86_RUNNER_CLIENT_H
#include <platform/x86_runner.h>
#include <platform/x86_vmenter.h>
/* One outstanding request per native executor. State persists across guest
 * destroy/recreate; never reset a sequence while its executor remains alive. */
bool aos_x86_runner_call(aos_x86_runner_t *, seL4_CPtr,
    const aos_x86_vmenter_entry_t *, aos_x86_vmenter_return_t *);
#endif
