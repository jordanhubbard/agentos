#ifndef AOS_ARM_RECREATE_H
#define AOS_ARM_RECREATE_H
#include <stdbool.h>
#include <stdint.h>
typedef enum {
    AOS_ARM_RECREATE_PAGING, AOS_ARM_RECREATE_RAM, AOS_ARM_RECREATE_EXECUTION,
    AOS_ARM_RECREATE_IMAGES, AOS_ARM_RECREATE_NATIVE_STATE,
    AOS_ARM_RECREATE_NET_REBIND, AOS_ARM_RECREATE_NET_ADOPT,
    AOS_ARM_RECREATE_BLK_REBIND, AOS_ARM_RECREATE_BLK_ADOPT,
    AOS_ARM_RECREATE_SERIAL_REBIND, AOS_ARM_RECREATE_CONSOLE,
    AOS_ARM_RECREATE_INPUT_REBIND, AOS_ARM_RECREATE_INPUT_ADOPT,
    AOS_ARM_RECREATE_GRAPHICS_STAGE, AOS_ARM_RECREATE_GRAPHICS_COMMIT,
    AOS_ARM_RECREATE_GRAPHICS_ADOPT, AOS_ARM_RECREATE_MEDIA,
    AOS_ARM_RECREATE_STEPS
} aos_arm_recreate_step_t;
enum {
    AOS_ARM_RECREATE_NET, AOS_ARM_RECREATE_BLK, AOS_ARM_RECREATE_SERIAL,
    AOS_ARM_RECREATE_INPUT, AOS_ARM_RECREATE_GRAPHICS, AOS_ARM_RECREATE_BACKENDS
};
/* Release callbacks revoke all of this guest's ordinary queue pools, then
 * graphics pools, execution, RAM and paging. They must tolerate partial
 * allocation/revocation and skip resources absent from the composition. */
enum {
    AOS_ARM_RELEASE_QUEUES, AOS_ARM_RELEASE_GRAPHICS,
    AOS_ARM_RELEASE_EXECUTION, AOS_ARM_RELEASE_RAM, AOS_ARM_RELEASE_PAGING,
    AOS_ARM_RECREATE_RELEASES
};
typedef struct {
    uint32_t generation, attempted_backends, release_next;
    aos_arm_recreate_step_t failed_step;
    bool failed, cleanup_pending;
} aos_arm_recreate_t;
typedef struct {
    bool (*step)(void *,aos_arm_recreate_step_t,uint32_t generation);
    void (*publish)(void *);
    /* Exclude execution and future callbacks without releasing live memory. */
    void (*retire)(void *);
    /* Includes service ABORT for staged but uncommitted graphics, otherwise
     * terminal detach. False retains ownership and blocks all revocation. */
    bool (*detach)(void *,unsigned backend);
    bool (*release)(void *,unsigned resource);
} aos_arm_recreate_ops_t;
/* Only after complete terminal teardown. Caller serializes against VM entry,
 * notifications that use old state, and lifecycle RPC during media loading.
 * Mark attempted ownership BEFORE a rebind, even when its reply fails.
 * Any failed attempt permanently prevents publication/retry on this state;
 * cleanup remains retryable. Do not guess generations after ambiguous IPC. */
bool aos_arm_recreate_run(aos_arm_recreate_t *,const aos_arm_recreate_ops_t *,
                          void *context,uint32_t enabled_backends);
bool aos_arm_recreate_cleanup(aos_arm_recreate_t *,const aos_arm_recreate_ops_t *,void *context);
#endif
