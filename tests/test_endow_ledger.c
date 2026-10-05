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
    {
        aos_endow_ledger_mark_t null_mark = aos_endow_ledger_mark(NULL);
        assert(null_mark.count == 0u && null_mark.dropped == 0u);
        aos_endow_ledger_rollback(NULL, null_mark);
    }
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
        aos_endow_ledger_mark_t mark = aos_endow_ledger_mark(&led);
        assert(mark.count == 3u && mark.dropped == 0u);
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
    {
        aos_endow_ledger_mark_t bogus = { led.count + 5u, 0u };
        aos_endow_ledger_rollback(&led, bogus);
        assert(led.count == 3u);
    }

    /* Overflow: refused and counted, never silently lost; and a rollback
     * of the SAME spawn's own overflows drops exactly those. */
    {
        aos_endow_ledger_t full;
        aos_endow_ledger_mark_t mark;
        aos_endow_ledger_init(&full);
        for (uint32_t i = 0u; i < AOS_ENDOW_LEDGER_MAX_ENTRIES; i++) {
            assert(aos_endow_ledger_record(&full, 1u, "c",
                                           AOS_AUTHORITY_KIND_FRAME, 0x1u, 1u)
                   == AOS_ENDOW_LEDGER_OK);
        }
        mark = aos_endow_ledger_mark(&full);
        assert(mark.count == AOS_ENDOW_LEDGER_MAX_ENTRIES && mark.dropped == 0u);
        assert(aos_endow_ledger_record(&full, 1u, "c",
                                       AOS_AUTHORITY_KIND_FRAME, 0x1u, 1u)
               == AOS_ENDOW_LEDGER_ERR_FULL);
        assert(full.count == AOS_ENDOW_LEDGER_MAX_ENTRIES);
        assert(full.dropped == 1u);
        aos_endow_ledger_rollback(&full, mark);
        assert(full.dropped == 0u);
        assert(full.count == AOS_ENDOW_LEDGER_MAX_ENTRIES);
    }

    /*
     * The sequence the rollback contract exists for: a SUCCESSFUL spawn
     * overflows the ledger (aos_child_spawn discards ERR_FULL, so the child
     * runs holding a capability this record does not list -- flagged only by
     * dropped != 0), and then an unrelated LATER spawn fails and rolls back.
     * The earlier flag must survive: clearing it would make the report claim
     * to be a complete account of a live domain's endowment while silently
     * omitting one of its capabilities.
     */
    {
        aos_endow_ledger_t led2;
        aos_endow_ledger_mark_t spawn_a, spawn_b;
        aos_endow_ledger_init(&led2);

        /* Spawn A: fills the table and overflows by two, then SUCCEEDS --
         * no rollback, because child A really is running. */
        spawn_a = aos_endow_ledger_mark(&led2);
        for (uint32_t i = 0u; i < AOS_ENDOW_LEDGER_MAX_ENTRIES + 2u; i++) {
            (void)aos_endow_ledger_record(&led2, 1u, "child_a",
                                          AOS_AUTHORITY_KIND_FRAME, 0x1u, 0u);
        }
        assert(spawn_a.dropped == 0u);
        assert(led2.count == AOS_ENDOW_LEDGER_MAX_ENTRIES);
        assert(led2.dropped == 2u);

        /* Spawn B: records nothing it can fit, overflows once more, FAILS. */
        spawn_b = aos_endow_ledger_mark(&led2);
        assert(spawn_b.dropped == 2u);
        assert(aos_endow_ledger_record(&led2, 2u, "child_b",
                                       AOS_AUTHORITY_KIND_NOTIFICATION, 0x1u, 0u)
               == AOS_ENDOW_LEDGER_ERR_FULL);
        assert(led2.dropped == 3u);
        aos_endow_ledger_rollback(&led2, spawn_b);

        /* B's own overflow is gone; A's incompleteness flag survives. */
        assert(led2.count == AOS_ENDOW_LEDGER_MAX_ENTRIES);
        assert(led2.dropped == 2u);

        /* And the merged report still carries no row for the child that
         * never started. */
        aos_authority_init(&snap);
        assert(aos_endow_ledger_merge(&led2, &snap) == AOS_ENDOW_LEDGER_OK);
        assert(snap.pd_count == 1u);
        assert(snap.pds[0].pd_index == 1u);
    }

    /*
     * Only badgeable kinds record a badge. seL4 ignores the badge argument
     * to seL4_CNode_Mint for a frame, so echoing the requested value into a
     * record whose purpose is accurate self-reporting would be a false
     * field.
     */
    {
        aos_endow_ledger_t b;
        aos_endow_ledger_init(&b);
        assert(aos_endow_ledger_record(&b, 1u, "c",
                   AOS_AUTHORITY_KIND_NOTIFICATION, 0x1u, 0xC417u)
               == AOS_ENDOW_LEDGER_OK);
        assert(aos_endow_ledger_record(&b, 1u, "c",
                   AOS_AUTHORITY_KIND_ENDPOINT, 0x3u, 0xC417u)
               == AOS_ENDOW_LEDGER_OK);
        assert(aos_endow_ledger_record(&b, 1u, "c",
                   AOS_AUTHORITY_KIND_FRAME, 0x3u, 0xC417u)
               == AOS_ENDOW_LEDGER_OK);
        assert(aos_endow_ledger_record(&b, 1u, "c",
                   AOS_AUTHORITY_KIND_CNODE, 0x3u, 0xC417u)
               == AOS_ENDOW_LEDGER_OK);
        /* An unrecognised kind keeps what the caller passed rather than
         * discarding it on a guess. */
        assert(aos_endow_ledger_record(&b, 1u, "c", 0xBEEFu, 0x3u, 0xC417u)
               == AOS_ENDOW_LEDGER_OK);
        assert(b.entries[0].badge == 0xC417u);  /* notification */
        assert(b.entries[1].badge == 0xC417u);  /* endpoint */
        assert(b.entries[2].badge == 0u);       /* frame -- carries none */
        assert(b.entries[3].badge == 0u);       /* cnode -- carries none */
        assert(b.entries[4].badge == 0xC417u);  /* unknown kind */
        /* The rights field is unaffected either way. */
        assert(b.entries[2].rights == 0x3u);
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
