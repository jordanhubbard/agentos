#ifndef AOS_PLATFORM_X86_RECREATE_H
#define AOS_PLATFORM_X86_RECREATE_H
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AOS_X86_RECREATE_OBJECTS,
    AOS_X86_RECREATE_MEMORY,
    AOS_X86_RECREATE_NATIVE_STATE,
    AOS_X86_RECREATE_NET_REBIND,
    AOS_X86_RECREATE_NET_ADOPT,
    AOS_X86_RECREATE_BLK_REBIND,
    AOS_X86_RECREATE_BLK_ADOPT,
    AOS_X86_RECREATE_SERIAL_REBIND,
    AOS_X86_RECREATE_CONSOLE,
    AOS_X86_RECREATE_BIND,
    AOS_X86_RECREATE_CPU,
    AOS_X86_RECREATE_STEPS
} aos_x86_recreate_step_t;

enum {
    AOS_X86_RECREATE_NET,
    AOS_X86_RECREATE_BLK,
    AOS_X86_RECREATE_SERIAL,
    AOS_X86_RECREATE_BACKENDS
};
/* Queue indices match the backend order above. Release execution before RAM. */
enum {
    AOS_X86_RECREATE_EXECUTION = AOS_X86_RECREATE_BACKENDS,
    AOS_X86_RECREATE_RAM,
    AOS_X86_RECREATE_RELEASES
};

typedef struct {
    uint32_t generation, attempted_backends, release_next;
    aos_x86_recreate_step_t failed_step;
    bool failed, cleanup_pending;
} aos_x86_recreate_t;

typedef struct {
    bool (*step)(void *, aos_x86_recreate_step_t, uint32_t generation);
    void (*publish)(void *);
    void (*retire)(void *);
    bool (*detach)(void *, unsigned backend);
    bool (*release)(void *, unsigned resource);
} aos_x86_recreate_ops_t;

/* Caller must have completed terminal teardown and must exclude VM entry.
 * REBIND attempts acquire cleanup responsibility even when their reply fails.
 * Success publishes once; any failure permanently rejects further runs on
 * this state. No generation guessing after ambiguous service outcomes. */
bool aos_x86_recreate_run(aos_x86_recreate_t *, const aos_x86_recreate_ops_t *, void *);
/* Retry partial cleanup, retaining ownership until all detaches acknowledge.
 * Release callbacks must tolerate retry after partial revocation. */
bool aos_x86_recreate_cleanup(aos_x86_recreate_t *, const aos_x86_recreate_ops_t *, void *);
#endif
