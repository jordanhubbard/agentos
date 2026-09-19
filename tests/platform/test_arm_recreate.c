#include <platform/arm_recreate.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int fail_step, blocked_detach, blocked_release;
    unsigned live, published, releases, steps;
    bool retired;
} fixture_t;
static bool step(void *context,aos_arm_recreate_step_t step,uint32_t generation)
{
    fixture_t *f=context;
    assert(generation && !f->published);
    ++f->steps;
    switch (step) {
    case AOS_ARM_RECREATE_NET_REBIND: f->live|=1u<<AOS_ARM_RECREATE_NET; break;
    case AOS_ARM_RECREATE_BLK_REBIND: f->live|=1u<<AOS_ARM_RECREATE_BLK; break;
    case AOS_ARM_RECREATE_SERIAL_REBIND: f->live|=1u<<AOS_ARM_RECREATE_SERIAL; break;
    case AOS_ARM_RECREATE_INPUT_REBIND: f->live|=1u<<AOS_ARM_RECREATE_INPUT; break;
    case AOS_ARM_RECREATE_GRAPHICS_STAGE: f->live|=1u<<AOS_ARM_RECREATE_GRAPHICS; break;
    default: break;
    }
    return (int)step!=f->fail_step;
}
static void publish(void *context) { ++((fixture_t *)context)->published; }
static void retire(void *context) { ((fixture_t *)context)->retired=true; }
static bool detach(void *context,unsigned backend)
{
    fixture_t *f=context;
    assert(f->retired && !f->releases && (f->live & (1u<<backend)));
    if ((int)backend==f->blocked_detach) return false;
    f->live &= ~(1u<<backend);
    return true;
}
static bool release(void *context,unsigned resource)
{
    fixture_t *f=context;
    assert(f->retired && !f->live && resource==f->releases);
    if ((int)resource==f->blocked_release) return false;
    ++f->releases;
    return true;
}
static const aos_arm_recreate_ops_t ops={step,publish,retire,detach,release};
static fixture_t clean(void) { return (fixture_t){.fail_step=-1,.blocked_detach=-1,.blocked_release=-1}; }
int main(void)
{
    const unsigned all=(1u<<AOS_ARM_RECREATE_BACKENDS)-1u;
    for (int failure=0;failure<AOS_ARM_RECREATE_STEPS;failure++) {
        aos_arm_recreate_t state={0};
        fixture_t f=clean(); f.fail_step=failure;
        assert(!aos_arm_recreate_run(&state,&ops,&f,all));
        assert(state.failed && !state.cleanup_pending && state.failed_step==(unsigned)failure);
        assert(!f.published && !f.live && f.releases==AOS_ARM_RECREATE_RELEASES);
        assert(f.steps==(unsigned)failure+1);
        assert(!aos_arm_recreate_run(&state,&ops,&f,all));
    }
    for (int blocked=0;blocked<AOS_ARM_RECREATE_BACKENDS;blocked++) {
        aos_arm_recreate_t state={0};
        fixture_t f=clean(); f.fail_step=AOS_ARM_RECREATE_MEDIA; f.blocked_detach=blocked;
        assert(!aos_arm_recreate_run(&state,&ops,&f,all));
        assert(state.cleanup_pending && !f.releases && !f.published);
        assert(!aos_arm_recreate_cleanup(&state,&ops,&f));
        f.blocked_detach=-1;
        assert(aos_arm_recreate_cleanup(&state,&ops,&f));
        assert(!f.live && !state.cleanup_pending && !f.published);
    }
    for (int blocked=0;blocked<AOS_ARM_RECREATE_RELEASES;blocked++) {
        aos_arm_recreate_t state={0};
        fixture_t f=clean(); f.fail_step=AOS_ARM_RECREATE_MEDIA; f.blocked_release=blocked;
        assert(!aos_arm_recreate_run(&state,&ops,&f,all));
        assert(state.cleanup_pending && state.release_next==(unsigned)blocked);
        f.blocked_release=-1;
        assert(aos_arm_recreate_cleanup(&state,&ops,&f));
        assert(f.releases==AOS_ARM_RECREATE_RELEASES && !f.published);
    }
    aos_arm_recreate_t state={0};
    fixture_t f=clean();
    assert(aos_arm_recreate_run(&state,&ops,&f,all));
    assert(f.published==1 && f.steps==AOS_ARM_RECREATE_STEPS && !f.releases && state.generation==1);
    /* Caller has completed teardown before the next generation. */
    f=clean();
    assert(aos_arm_recreate_run(&state,&ops,&f,1u<<AOS_ARM_RECREATE_SERIAL));
    assert(f.live==(1u<<AOS_ARM_RECREATE_SERIAL) && f.steps==8 && state.generation==2);
    f=clean(); state.generation=UINT32_MAX;
    assert(!aos_arm_recreate_run(&state,&ops,&f,all) && !f.steps);
    state=(aos_arm_recreate_t){0};
    assert(!aos_arm_recreate_run(&state,&ops,&f,all+1u) && !f.steps);
    puts("PASS: ARM reconstruction publishes only complete state and retains failed cleanup ownership");
}
