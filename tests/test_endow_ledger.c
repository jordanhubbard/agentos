/*
 * Host test: endowment-delta ledger (platform/inspect/endow_ledger.c) and
 * its merge into T4's authority snapshot. No seL4; logic only.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/authority.h"
#include "platform/endow_ledger.h"

int main(void)
{
    aos_endow_ledger_t led;
    aos_authority_snapshot_t snap;

    aos_endow_ledger_init(&led);
    assert(led.version == AOS_ENDOW_LEDGER_VERSION);
    assert(led.count == 0u);
    assert(led.dropped == 0u);
    assert(aos_endow_ledger_validate(&led) == AOS_ENDOW_LEDGER_OK);

    /* NULL ledger is refused, never crashes: a delegator that does not
     * want to self-report passes NULL. */
    assert(aos_endow_ledger_record(NULL, 1u, "x", AOS_AUTHORITY_KIND_FRAME, 1u, 7u)
           == AOS_ENDOW_LEDGER_ERR_NULL);
    aos_endow_ledger_init(NULL);
    aos_endow_ledger_rollback(NULL, 0u);
    assert(aos_endow_ledger_mark(NULL) == 0u);
    assert(aos_endow_ledger_validate(NULL) == AOS_ENDOW_LEDGER_ERR_NULL);

    /* A version-mismatched ledger is refused on both record and merge. */
    {
        aos_endow_ledger_t bad;
        aos_endow_ledger_init(&bad);
        bad.version = AOS_ENDOW_LEDGER_VERSION + 1u;
        assert(aos_endow_ledger_record(&bad, 1u, "c", AOS_AUTHORITY_KIND_FRAME, 1u, 1u)
               == AOS_ENDOW_LEDGER_ERR_VERSION);
        aos_authority_init(&snap);
        assert(aos_endow_ledger_merge(&bad, &snap) == AOS_ENDOW_LEDGER_ERR_VERSION);
    }

    /* Record two caps for one child. */
    assert(aos_endow_ledger_record(&led, 1u, "child_spawn_child",
                                   AOS_AUTHORITY_KIND_NOTIFICATION, 0x1u, 0xC417u)
           == AOS_ENDOW_LEDGER_OK);
    assert(aos_endow_ledger_record(&led, 1u, "child_spawn_child",
                                   AOS_AUTHORITY_KIND_FRAME, 0x3u, 0xC417u)
           == AOS_ENDOW_LEDGER_OK);
    assert(led.count == 2u);
    assert(led.entries[0].kind == (uint32_t)AOS_AUTHORITY_KIND_NOTIFICATION);
    assert(led.entries[0].rights == 0x1u);
    assert(led.entries[0].badge == 0xC417u);
    assert(strcmp((const char *)led.entries[1].child_name, "child_spawn_child") == 0);

    /* Merge: one row, counts match exactly what was recorded. */
    aos_authority_init(&snap);
    assert(aos_endow_ledger_merge(&led, &snap) == AOS_ENDOW_LEDGER_OK);
    assert(snap.pd_count == 1u);
    assert(snap.pds[0].pd_index == 1u);
    assert(strcmp((const char *)snap.pds[0].name, "child_spawn_child") == 0);
    assert(snap.pds[0].counts[AOS_AUTHORITY_KIND_NOTIFICATION] == 1u);
    assert(snap.pds[0].counts[AOS_AUTHORITY_KIND_FRAME] == 1u);
    assert(snap.pds[0].counts[AOS_AUTHORITY_KIND_TCB] == 0u);
    assert(snap.total_recorded == 2u);
    assert(snap.truncated_adds == 0u);
    assert(snap.saturated == 0u);
    assert(aos_authority_validate(&snap) == AOS_AUTHORITY_OK);

    /* NULL snapshot is refused. */
    assert(aos_endow_ledger_merge(&led, NULL) == AOS_ENDOW_LEDGER_ERR_NULL);

    /* A second child gets its own row; a kind outside the enumeration is
     * bucketed as OTHER by aos_authority_add(), never dropped. */
    assert(aos_endow_ledger_record(&led, 2u, "other_child", 0xBEEFu, 0x1u, 1u)
           == AOS_ENDOW_LEDGER_OK);
    aos_authority_init(&snap);
    assert(aos_endow_ledger_merge(&led, &snap) == AOS_ENDOW_LEDGER_OK);
    assert(snap.pd_count == 2u);
    assert(snap.pds[1].counts[AOS_AUTHORITY_KIND_OTHER] == 1u);

    /* mark/rollback: a spawn that fails leaves nothing behind. This is the
     * property that keeps the report honest -- a child that never started
     * was never endowed, whatever partial mints the kernel accepted. */
    {
        uint32_t mark = aos_endow_ledger_mark(&led);
        assert(mark == 3u);
        assert(aos_endow_ledger_record(&led, 3u, "doomed_child",
                                       AOS_AUTHORITY_KIND_ENDPOINT, 0x3u, 9u)
               == AOS_ENDOW_LEDGER_OK);
        assert(led.count == 4u);
        aos_endow_ledger_rollback(&led, mark);
        assert(led.count == 3u);
        /* The abandoned entry is zeroed, not merely hidden behind count. */
        assert(led.entries[3].child_index == 0u);
        assert(led.entries[3].child_name[0] == 0u);

        aos_authority_init(&snap);
        assert(aos_endow_ledger_merge(&led, &snap) == AOS_ENDOW_LEDGER_OK);
        assert(snap.pd_count == 2u);
        for (uint32_t i = 0u; i < snap.pd_count; i++) {
            assert(snap.pds[i].pd_index != 3u);
        }
    }

    /* Rollback to a mark beyond the current count is a no-op, not a
     * truncation of live rows into negative territory. */
    aos_endow_ledger_rollback(&led, led.count + 5u);
    assert(led.count == 3u);

    /* Overflow: refused and counted, never silently lost; and a rollback
     * clears the "entries were lost" claim because nothing was granted. */
    {
        aos_endow_ledger_t full;
        uint32_t mark;
        aos_endow_ledger_init(&full);
        for (uint32_t i = 0u; i < AOS_ENDOW_LEDGER_MAX_ENTRIES; i++) {
            assert(aos_endow_ledger_record(&full, 1u, "c",
                                           AOS_AUTHORITY_KIND_FRAME, 0x1u, 1u)
                   == AOS_ENDOW_LEDGER_OK);
        }
        mark = aos_endow_ledger_mark(&full);
        assert(aos_endow_ledger_record(&full, 1u, "c",
                                       AOS_AUTHORITY_KIND_FRAME, 0x1u, 1u)
               == AOS_ENDOW_LEDGER_ERR_FULL);
        assert(full.count == AOS_ENDOW_LEDGER_MAX_ENTRIES);
        assert(full.dropped == 1u);
        aos_endow_ledger_rollback(&full, mark);
        assert(full.dropped == 0u);
        assert(full.count == AOS_ENDOW_LEDGER_MAX_ENTRIES);
    }

    /* A name longer than the field is truncated and still NUL-terminated. */
    {
        aos_endow_ledger_t t;
        aos_endow_ledger_init(&t);
        assert(aos_endow_ledger_record(&t, 1u,
                   "an_extremely_long_child_domain_name_well_past_the_field",
                   AOS_AUTHORITY_KIND_CNODE, 0xFu, 0u) == AOS_ENDOW_LEDGER_OK);
        assert(t.entries[0].child_name[AOS_AUTHORITY_NAME_LEN - 1u] == 0u);
        assert(strlen((const char *)t.entries[0].child_name)
               == AOS_AUTHORITY_NAME_LEN - 1u);
    }

    printf("test_endow_ledger: OK\n");
    return 0;
}
