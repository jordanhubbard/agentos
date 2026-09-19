#include <platform/arm_recreate.h>

static bool valid(const aos_arm_recreate_ops_t *ops)
{
    return ops && ops->step && ops->publish && ops->retire && ops->detach && ops->release;
}
static unsigned backend(aos_arm_recreate_step_t step)
{
    switch (step) {
    case AOS_ARM_RECREATE_NET_REBIND: case AOS_ARM_RECREATE_NET_ADOPT:
        return AOS_ARM_RECREATE_NET;
    case AOS_ARM_RECREATE_BLK_REBIND: case AOS_ARM_RECREATE_BLK_ADOPT:
        return AOS_ARM_RECREATE_BLK;
    case AOS_ARM_RECREATE_SERIAL_REBIND: case AOS_ARM_RECREATE_CONSOLE:
        return AOS_ARM_RECREATE_SERIAL;
    case AOS_ARM_RECREATE_INPUT_REBIND: case AOS_ARM_RECREATE_INPUT_ADOPT:
        return AOS_ARM_RECREATE_INPUT;
    case AOS_ARM_RECREATE_GRAPHICS_STAGE: case AOS_ARM_RECREATE_GRAPHICS_COMMIT:
    case AOS_ARM_RECREATE_GRAPHICS_ADOPT: return AOS_ARM_RECREATE_GRAPHICS;
    default: return AOS_ARM_RECREATE_BACKENDS;
    }
}
bool aos_arm_recreate_cleanup(aos_arm_recreate_t *s,const aos_arm_recreate_ops_t *ops,void *context)
{
    if (!s || !valid(ops)) return false;
    if (!s->cleanup_pending) return true;
    ops->retire(context);
    for (unsigned i=0;i<AOS_ARM_RECREATE_BACKENDS;i++) {
        if (!(s->attempted_backends & (1u<<i))) continue;
        if (!ops->detach(context,i)) return false;
        s->attempted_backends &= ~(1u<<i);
    }
    while (s->release_next<AOS_ARM_RECREATE_RELEASES) {
        if (!ops->release(context,s->release_next)) return false;
        ++s->release_next;
    }
    s->cleanup_pending=false;
    return true;
}
bool aos_arm_recreate_run(aos_arm_recreate_t *s,const aos_arm_recreate_ops_t *ops,
                          void *context,uint32_t enabled)
{
    if (!s || !valid(ops) || enabled & ~((1u<<AOS_ARM_RECREATE_BACKENDS)-1u) ||
        s->failed || s->cleanup_pending || s->attempted_backends ||
        s->generation==UINT32_MAX) return false;
    ++s->generation;
    s->release_next=0;
    for (unsigned i=0;i<AOS_ARM_RECREATE_STEPS;i++) {
        aos_arm_recreate_step_t step=(aos_arm_recreate_step_t)i;
        unsigned owner=backend(step);
        if (owner<AOS_ARM_RECREATE_BACKENDS) {
            if (!(enabled & (1u<<owner))) continue;
            s->attempted_backends |= 1u<<owner;
        }
        if (!ops->step(context,step,s->generation)) {
            s->failed_step=step;
            s->failed=s->cleanup_pending=true;
            (void)aos_arm_recreate_cleanup(s,ops,context);
            return false;
        }
    }
    ops->publish(context);
    s->attempted_backends=0;
    return true;
}
