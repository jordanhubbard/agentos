#include <platform/x86_recreate.h>

static bool valid_ops(const aos_x86_recreate_ops_t *ops)
{
    return ops && ops->step && ops->publish && ops->retire && ops->detach && ops->release;
}

bool aos_x86_recreate_cleanup(aos_x86_recreate_t *s,
                              const aos_x86_recreate_ops_t *ops, void *context)
{
    if (!s || !valid_ops(ops)) return false;
    if (!s->cleanup_pending) return true;
    ops->retire(context);
    for (unsigned i = 0; i < AOS_X86_RECREATE_BACKENDS; i++) {
        if (!(s->attempted_backends & (1u << i))) continue;
        if (!ops->detach(context, i)) return false;
        s->attempted_backends &= ~(1u << i);
    }
    while (s->release_next < AOS_X86_RECREATE_RELEASES) {
        if (!ops->release(context, s->release_next)) return false;
        s->release_next++;
    }
    s->cleanup_pending = false;
    return true;
}

bool aos_x86_recreate_run(aos_x86_recreate_t *s,
                          const aos_x86_recreate_ops_t *ops, void *context)
{
    if (!s || !valid_ops(ops) || s->failed || s->cleanup_pending ||
            s->generation == UINT32_MAX) return false;
    s->generation++;
    s->release_next = 0u;
    for (unsigned i = 0; i < AOS_X86_RECREATE_STEPS; i++) {
        aos_x86_recreate_step_t step = (aos_x86_recreate_step_t)i;
        if (step == AOS_X86_RECREATE_NET_REBIND)
            s->attempted_backends |= 1u << AOS_X86_RECREATE_NET;
        if (step == AOS_X86_RECREATE_BLK_REBIND)
            s->attempted_backends |= 1u << AOS_X86_RECREATE_BLK;
        if (step == AOS_X86_RECREATE_SERIAL_REBIND)
            s->attempted_backends |= 1u << AOS_X86_RECREATE_SERIAL;
        if (!ops->step(context, step, s->generation)) {
            s->failed_step = step;
            s->failed = s->cleanup_pending = true;
            (void)aos_x86_recreate_cleanup(s, ops, context);
            return false;
        }
    }
    ops->publish(context);
    s->attempted_backends = 0u;
    return true;
}
