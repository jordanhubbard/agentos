/* Host test: authority snapshot ABI and builder. No seL4; logic only. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform/authority.h"

int main(void)
{
    aos_authority_snapshot_t s;
    aos_authority_init(&s);
    assert(s.version == AOS_AUTHORITY_VERSION);
    assert(s.pd_count == 0u);
    assert(s.truncated_adds == 0u);
    assert(aos_authority_validate(&s) == AOS_AUTHORITY_OK);

    /* First add creates the domain row. */
    assert(aos_authority_add(&s, 3u, "serial_pd", AOS_AUTHORITY_KIND_FRAME) == AOS_AUTHORITY_OK);
    assert(s.pd_count == 1u);
    assert(s.pds[0].pd_index == 3u);
    assert(strcmp((const char *)s.pds[0].name, "serial_pd") == 0);
    assert(s.pds[0].counts[AOS_AUTHORITY_KIND_FRAME] == 1u);

    /* Same domain, same kind: increments, no new row. */
    assert(aos_authority_add(&s, 3u, "serial_pd", AOS_AUTHORITY_KIND_FRAME) == AOS_AUTHORITY_OK);
    assert(s.pd_count == 1u);
    assert(s.pds[0].counts[AOS_AUTHORITY_KIND_FRAME] == 2u);

    /* Same domain, different kind. */
    assert(aos_authority_add(&s, 3u, "serial_pd", AOS_AUTHORITY_KIND_IRQ_HANDLER) == AOS_AUTHORITY_OK);
    assert(s.pds[0].counts[AOS_AUTHORITY_KIND_IRQ_HANDLER] == 1u);
    assert(s.pd_count == 1u);

    /* A second domain gets its own row. */
    assert(aos_authority_add(&s, 7u, "net_virt", AOS_AUTHORITY_KIND_ENDPOINT) == AOS_AUTHORITY_OK);
    assert(s.pd_count == 2u);
    assert(s.pds[1].pd_index == 7u);

    /* An unknown kind is bucketed as OTHER, never dropped. */
    assert(aos_authority_add(&s, 7u, "net_virt", 0xBEEFu) == AOS_AUTHORITY_OK);
    assert(s.pds[1].counts[AOS_AUTHORITY_KIND_OTHER] == 1u);

    /* Totals reconcile: every add is counted exactly once somewhere. */
    uint32_t sum = 0u;
    for (uint32_t i = 0u; i < s.pd_count; i++)
        for (uint32_t k = 0u; k < AOS_AUTHORITY_KIND_COUNT; k++)
            sum += s.pds[i].counts[k];
    assert(sum == 5u);
    assert(s.total_recorded == 5u);

    /* Overflowing the domain table is reported, not silently dropped. */
    {
        aos_authority_snapshot_t o;
        aos_authority_init(&o);
        for (uint32_t i = 0u; i < AOS_AUTHORITY_MAX_PDS + 4u; i++) {
            char nm[8];
            nm[0] = 'p'; nm[1] = (char)('0' + (int)(i % 10u)); nm[2] = '\0';
            (void)aos_authority_add(&o, i, nm, AOS_AUTHORITY_KIND_TCB);
        }
        assert(o.pd_count == AOS_AUTHORITY_MAX_PDS);
        assert(o.truncated_adds == 4u);
        assert(aos_authority_validate(&o) == AOS_AUTHORITY_OK);
    }

    /* A count that saturates uint16 is flagged, not wrapped. */
    {
        aos_authority_snapshot_t t;
        aos_authority_init(&t);
        for (uint32_t i = 0u; i < 70000u; i++)
            (void)aos_authority_add(&t, 1u, "busy", AOS_AUTHORITY_KIND_FRAME);
        assert(t.pds[0].counts[AOS_AUTHORITY_KIND_FRAME] == 0xFFFFu);
        assert(t.saturated != 0u);
    }

    /* validate rejects a bad version and an impossible pd_count. */
    {
        aos_authority_snapshot_t b = s;
        b.version = AOS_AUTHORITY_VERSION + 1u;
        assert(aos_authority_validate(&b) == AOS_AUTHORITY_ERR_VERSION);
        aos_authority_snapshot_t c = s;
        c.pd_count = AOS_AUTHORITY_MAX_PDS + 1u;
        assert(aos_authority_validate(&c) == AOS_AUTHORITY_ERR_INVALID);
    }

    /* format writes key=value lines and never overruns. */
    {
        char buf[2048];
        int n = aos_authority_format(&s, buf, sizeof(buf));
        assert(n > 0);
        assert(strstr(buf, "serial_pd") != NULL);
        assert(strstr(buf, "frame=2") != NULL);
        char tiny[8];
        assert(aos_authority_format(&s, tiny, sizeof(tiny)) == AOS_AUTHORITY_ERR_TRUNC);
    }

    printf("test_authority_snapshot: PASS\n");
    return 0;
}
