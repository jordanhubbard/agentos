#include <platform/x86_recreate.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int fail_step, fail_detach, fail_release;
    bool service_commits, cleanup_commits, retired, objects, memory, published;
    uint32_t generation, owned_backends;
    unsigned steps, retires, detaches[3], releases[5], released;
} model_t;

static model_t model(void)
{
    return (model_t){.fail_step = -1, .fail_detach = -1, .fail_release = -1,
        .service_commits = true};
}

static bool step(void *context, aos_x86_recreate_step_t s, uint32_t generation)
{
    model_t *m = context;
    assert(!m->retired && !m->published);
    assert((unsigned)s == m->steps++);
    assert(generation && (!m->generation || generation == m->generation));
    m->generation = generation;
    if (s == AOS_X86_RECREATE_OBJECTS) m->objects = true;
    if (s == AOS_X86_RECREATE_MEMORY) {
        assert(m->objects);
        m->memory = true;
    }
    if (s >= AOS_X86_RECREATE_NATIVE_STATE) assert(m->objects && m->memory);
    int backend = s == AOS_X86_RECREATE_NET_REBIND ? AOS_X86_RECREATE_NET :
        s == AOS_X86_RECREATE_BLK_REBIND ? AOS_X86_RECREATE_BLK :
        s == AOS_X86_RECREATE_SERIAL_REBIND ? AOS_X86_RECREATE_SERIAL : -1;
    /* A failed reply does not reveal whether capability transfer committed. */
    if (backend >= 0 && (m->service_commits || (int)s != m->fail_step))
        m->owned_backends |= 1u << backend;
    if (s == AOS_X86_RECREATE_NET_ADOPT) assert(m->owned_backends & 1u);
    if (s == AOS_X86_RECREATE_BLK_ADOPT) assert(m->owned_backends & 2u);
    if (s == AOS_X86_RECREATE_CONSOLE) assert(m->owned_backends & 4u);
    return (int)s != m->fail_step;
}

static void publish(void *context)
{
    model_t *m = context;
    assert(m->steps == AOS_X86_RECREATE_STEPS && m->owned_backends == 7u);
    assert(m->objects && m->memory && !m->retired && !m->published);
    m->published = true;
}

static void retire(void *context)
{
    model_t *m = context;
    assert(!m->published);
    m->retired = true;
    m->retires++;
}

static bool detach(void *context, unsigned backend)
{
    model_t *m = context;
    assert(m->retired && !m->published && backend < 3u && !m->released);
    m->detaches[backend]++;
    if ((int)backend == m->fail_detach) {
        if (m->cleanup_commits) m->owned_backends &= ~(1u << backend);
        m->fail_detach = -1;
        return false; /* BUSY, malformed reply or transport failure */
    }
    m->owned_backends &= ~(1u << backend);
    return true;
}

static bool release(void *context, unsigned resource)
{
    model_t *m = context;
    /* Retired bus and acknowledged backend ownership are mandatory. */
    assert(m->retired && !m->published && !m->owned_backends);
    assert(resource < 5u && resource == m->released);
    m->releases[resource]++;
    if ((int)resource == m->fail_release) {
        if (m->cleanup_commits && resource == AOS_X86_RECREATE_EXECUTION)
            m->objects = false;
        if (m->cleanup_commits && resource == AOS_X86_RECREATE_RAM)
            m->memory = false;
        m->fail_release = -1;
        return false;
    }
    if (resource == AOS_X86_RECREATE_EXECUTION) m->objects = false;
    if (resource == AOS_X86_RECREATE_RAM) {
        assert(!m->objects);
        m->memory = false;
    }
    m->released++;
    return true;
}

static const aos_x86_recreate_ops_t ops = {
    .step = step, .publish = publish, .retire = retire,
    .detach = detach, .release = release,
};

static void assert_terminal(aos_x86_recreate_t *s, model_t *m)
{
    assert(s->failed && !s->cleanup_pending && !s->attempted_backends);
    assert(!m->published && !m->objects && !m->memory && !m->owned_backends);
    assert(m->released == AOS_X86_RECREATE_RELEASES);
    model_t before;
    memcpy(&before, m, sizeof(before));
    uint32_t generation = s->generation;
    assert(!aos_x86_recreate_run(s, &ops, m));
    assert(aos_x86_recreate_cleanup(s, &ops, m));
    assert(s->generation == generation && !memcmp(&before, m, sizeof(before)));
}

int main(void)
{
    aos_x86_recreate_t s = {0};
    for (unsigned generation = 1; generation <= 2; generation++) {
        /* The caller has completed normal teardown before the next run. */
        model_t m = model();
        assert(aos_x86_recreate_run(&s, &ops, &m));
        assert(s.generation == generation && m.generation == generation);
        assert(m.published && !m.retires && !m.released);
        assert(!s.failed && !s.cleanup_pending && !s.attempted_backends);
    }

    for (int fail = 0; fail < AOS_X86_RECREATE_STEPS; fail++) {
        for (unsigned committed = 0; committed < 2u; committed++) {
            s = (aos_x86_recreate_t){0};
            model_t m = model();
            m.fail_step = fail;
            m.service_commits = committed;
            assert(!aos_x86_recreate_run(&s, &ops, &m));
            assert(s.generation == 1u && (int)s.failed_step == fail);
            assert(m.steps == (unsigned)fail + 1u && m.retires == 1u);
            assert(m.detaches[0] == (unsigned)(fail >= AOS_X86_RECREATE_NET_REBIND));
            assert(m.detaches[1] == (unsigned)(fail >= AOS_X86_RECREATE_BLK_REBIND));
            assert(m.detaches[2] == (unsigned)(fail >= AOS_X86_RECREATE_SERIAL_REBIND));
            assert_terminal(&s, &m);
        }
    }

    /* Every detach and revocation can pause cleanup. No earlier completed
     * resource is released again; the rejected operation alone is retried. */
    for (unsigned failure = 0; failure < 8u; failure++) {
      for (unsigned committed = 0; committed < 2u; committed++) {
        s = (aos_x86_recreate_t){0};
        model_t m = model();
        m.fail_step = AOS_X86_RECREATE_CPU;
        m.cleanup_commits = committed;
        if (failure < 3u) m.fail_detach = (int)failure;
        else m.fail_release = (int)failure - 3;
        assert(!aos_x86_recreate_run(&s, &ops, &m));
        assert(s.failed && s.cleanup_pending && !m.published);
        if (failure < 3u) {
            assert(!m.released && m.objects && m.memory);
            assert(s.attempted_backends & (1u << failure));
        } else {
            assert(!s.attempted_backends && s.release_next == failure - 3u);
        }
        unsigned steps = m.steps;
        assert(!aos_x86_recreate_run(&s, &ops, &m));
        assert(m.steps == steps && s.generation == 1u);
        assert(aos_x86_recreate_cleanup(&s, &ops, &m));
        for (unsigned i = 0; i < 3u; i++)
            assert(m.detaches[i] == (failure == i ? 2u : 1u));
        for (unsigned i = 0; i < 5u; i++)
            assert(m.releases[i] == (failure == i + 3u ? 2u : 1u));
        assert_terminal(&s, &m);
      }
    }

    s = (aos_x86_recreate_t){.generation = UINT32_MAX};
    model_t m = model(), before;
    memcpy(&before, &m, sizeof(before));
    assert(!aos_x86_recreate_run(&s, &ops, &m));
    assert(s.generation == UINT32_MAX && !memcmp(&m, &before, sizeof(m)));
    assert(!aos_x86_recreate_run(NULL, &ops, &m));
    assert(!aos_x86_recreate_cleanup(NULL, &ops, &m));
    s = (aos_x86_recreate_t){0};
    aos_x86_recreate_ops_t invalid = ops;
    invalid.detach = NULL;
    assert(!aos_x86_recreate_run(&s, &invalid, &m));
    assert(!s.generation && !memcmp(&m, &before, sizeof(m)));
    puts("PASS: 11 reconstruction failures, ambiguous REBIND ownership, 8 cleanup retry boundaries, terminal rejection and two generations");
    return 0;
}
