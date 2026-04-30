/*
 * test_cap_accounting.c — API tests for root-task capability accounting
 */

#ifdef AGENTOS_TEST_HOST

#include "framework.h"
#include "cap_accounting.h"

#include "../../kernel/agentos-root-task/src/cap_accounting.c"

static void test_init_seeds_root_caps(void)
{
    seL4_BootInfo bi;
    memset(&bi, 0, sizeof(bi));

    cap_acct_init(&bi);

    ASSERT_EQ(cap_acct_count(), 3u, "cap_acct_init: seeds 3 root caps");

    const cap_acct_entry_t *cnode = cap_acct_get(0);
    const cap_acct_entry_t *vspace = cap_acct_get(1);
    const cap_acct_entry_t *tcb = cap_acct_get(2);

    ASSERT_EQ(cnode->cap, seL4_CapInitThreadCNode, "cap_acct_init: records root CNode cap");
    ASSERT_EQ(cnode->obj_type, seL4_CapTableObject, "cap_acct_init: root CNode object type");
    ASSERT_TRUE(strcmp(cnode->name, "root-cnode") == 0, "cap_acct_init: root CNode name");
    ASSERT_EQ(vspace->cap, seL4_CapInitThreadVSpace, "cap_acct_init: records root VSpace cap");
    ASSERT_EQ(tcb->cap, seL4_CapInitThreadTCB, "cap_acct_init: records root TCB cap");
    ASSERT_EQ(tcb->pd_index, 0u, "cap_acct_init: root caps owned by PD 0");
}

static void test_record_stores_entry(void)
{
    cap_acct_init((const seL4_BootInfo *)0);

    int rc = cap_acct_record(seL4_CapInitThreadCNode, 42u,
                             seL4_EndpointObject, 7u, "worker-ep");
    const cap_acct_entry_t *e = cap_acct_get(cap_acct_count() - 1u);

    ASSERT_EQ(rc, 0, "cap_acct_record: returns success");
    ASSERT_EQ(cap_acct_count(), 4u, "cap_acct_record: increments count");
    ASSERT_EQ(e->cap, 42u, "cap_acct_record: stores cap");
    ASSERT_EQ(e->obj_type, seL4_EndpointObject, "cap_acct_record: stores object type");
    ASSERT_EQ(e->pd_index, 7u, "cap_acct_record: stores PD index");
    ASSERT_TRUE(strcmp(e->name, "worker-ep") == 0, "cap_acct_record: stores name");
}

static void test_record_truncates_name(void)
{
    cap_acct_init((const seL4_BootInfo *)0);
    cap_acct_record(seL4_CapNull, 50u, seL4_TCBObject, 1u,
                    "abcdefghijklmnop-overflow");

    const cap_acct_entry_t *e = cap_acct_get(cap_acct_count() - 1u);
    ASSERT_EQ(strlen(e->name), 15u, "cap_acct_record: truncates name to 15 chars");
    ASSERT_TRUE(strncmp(e->name, "abcdefghijklmno", 15u) == 0,
                "cap_acct_record: truncated name content matches");
}

static void test_record_null_name(void)
{
    cap_acct_init((const seL4_BootInfo *)0);
    cap_acct_record(seL4_CapNull, 51u, seL4_TCBObject, 2u,
                    (const char *)0);

    const cap_acct_entry_t *e = cap_acct_get(cap_acct_count() - 1u);
    ASSERT_EQ(e->name[0], 0u, "cap_acct_record: NULL name becomes empty string");
    ASSERT_EQ(e->cap, 51u, "cap_acct_record: NULL name still records cap");
}

static void test_get_bounds(void)
{
    cap_acct_init((const seL4_BootInfo *)0);

    ASSERT_TRUE(cap_acct_get(0) != (const cap_acct_entry_t *)0,
                "cap_acct_get: in-range index returns entry");
    ASSERT_TRUE(cap_acct_get(cap_acct_count()) == (const cap_acct_entry_t *)0,
                "cap_acct_get: index == count returns NULL");
}

static void test_table_full(void)
{
    cap_acct_init((const seL4_BootInfo *)0);

    while (cap_acct_count() < CAP_ACCT_MAX_ENTRIES) {
        uint32_t n = cap_acct_count();
        int rc = cap_acct_record(seL4_CapNull, (seL4_CPtr)(1000u + n),
                                 seL4_EndpointObject, 1u, "fill");
        if (rc != 0) {
            break;
        }
    }

    ASSERT_EQ(cap_acct_count(), CAP_ACCT_MAX_ENTRIES,
              "cap_acct_record: table reaches maximum size");
    int rc = cap_acct_record(seL4_CapNull, 9999u, seL4_TCBObject, 1u, "overflow");
    ASSERT_TRUE(rc == -1, "cap_acct_record: returns -1 when table is full");
    ASSERT_EQ(cap_acct_count(), CAP_ACCT_MAX_ENTRIES,
              "cap_acct_record: full table does not increment count");
}

int main(void)
{
    TAP_PLAN(22);

    test_init_seeds_root_caps();
    test_record_stores_entry();
    test_record_truncates_name();
    test_record_null_name();
    test_get_bounds();
    test_table_full();

    return tap_exit();
}

#else
int main(void) { return 0; }
#endif
