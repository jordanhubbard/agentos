/*
 * test_nameserver.c — API tests for the production NameServer PD
 */

#ifdef AGENTOS_TEST_HOST

#include "framework.h"

uintptr_t log_drain_rings_vaddr;

void agentos_log_boot(const char *pd_name) { (void)pd_name; }
void agentos_log_channel(const char *pd, uint32_t ch) { (void)pd; (void)ch; }

#include "../../kernel/agentos-root-task/src/nameserver.c"

static void req_set(sel4_msg_t *req, uint32_t off, uint32_t value)
{
    rep_u32(req, off, value);
}

static uint32_t reply_get(const sel4_msg_t *rep, uint32_t off)
{
    return msg_u32(rep, off);
}

static void reset_nameserver(void)
{
    mock_mr_clear();
    ns_registry_shmem_vaddr = 0;
    nameserver_pd_init();
}

static void dispatch(uint32_t badge, const sel4_msg_t *req, sel4_msg_t *rep)
{
    memset(rep, 0, sizeof(*rep));
    (void)nameserver_h_dispatch((sel4_badge_t)badge, req, rep, (void *)0);
}

static void register_req(sel4_msg_t *req,
                         const char *name,
                         uint32_t channel_id,
                         uint32_t pd_id,
                         uint32_t cap_classes,
                         uint32_t version)
{
    memset(req, 0, sizeof(*req));
    req_set(req, 0, OP_NS_REGISTER);
    req_set(req, 4, channel_id);
    req_set(req, 8, pd_id);
    req_set(req, 12, cap_classes);
    req_set(req, 16, version);
    ns_pack_name(name, 5);
}

static void lookup_req(sel4_msg_t *req, uint32_t op, const char *name)
{
    memset(req, 0, sizeof(*req));
    req_set(req, 0, op);
    ns_pack_name(name, 1);
}

static void test_health_empty(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    reset_nameserver();

    req_set(&req, 0, OP_NS_HEALTH);
    dispatch(0u, &req, &rep);

    ASSERT_EQ(reply_get(&rep, 0), NS_OK, "nameserver: HEALTH returns OK");
    ASSERT_EQ(reply_get(&rep, 4), 0u, "nameserver: HEALTH count starts at 0");
    ASSERT_EQ(reply_get(&rep, 8), NS_VERSION, "nameserver: HEALTH returns version");
}

static void test_lookup_empty_name_bad_args(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    reset_nameserver();

    req_set(&req, 0, OP_NS_LOOKUP);
    mock_mr_clear();
    dispatch(0u, &req, &rep);

    ASSERT_EQ(reply_get(&rep, 0), NS_ERR_BAD_ARGS,
              "nameserver: LOOKUP empty name returns BAD_ARGS");
}

static void test_register_service(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    reset_nameserver();

    register_req(&req, "vfs", 19u, TRACE_PD_VFS_SERVER, CAP_CLASS_FS, 2u);
    dispatch(0u, &req, &rep);

    ASSERT_EQ(reply_get(&rep, 0), NS_OK, "nameserver: REGISTER returns OK");
    ASSERT_EQ(rep.length, 4u, "nameserver: REGISTER reply length is 4");

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, OP_NS_HEALTH);
    dispatch(0u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 4), 1u, "nameserver: REGISTER increments health count");
}

static void test_duplicate_register(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    reset_nameserver();

    register_req(&req, "vfs", 19u, TRACE_PD_VFS_SERVER, CAP_CLASS_FS, 2u);
    dispatch(0u, &req, &rep);
    register_req(&req, "vfs", 20u, TRACE_PD_NET_SERVER, CAP_CLASS_NET, 1u);
    dispatch(0u, &req, &rep);

    ASSERT_EQ(reply_get(&rep, 0), NS_ERR_DUPLICATE,
              "nameserver: duplicate REGISTER returns DUPLICATE");
}

static void test_register_table_full(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    char name[16];
    bool all_ok = true;
    reset_nameserver();

    for (uint32_t i = 0; i < NS_MAX_ENTRIES; i++) {
        snprintf(name, sizeof(name), "svc%02u", i);
        register_req(&req, name, 100u + i, TRACE_PD_VFS_SERVER, CAP_CLASS_FS, 1u);
        dispatch(0u, &req, &rep);
        if (reply_get(&rep, 0) != NS_OK) {
            all_ok = false;
        }
    }

    ASSERT_TRUE(all_ok, "nameserver: REGISTER fills every registry slot");

    register_req(&req, "overflow", 999u, TRACE_PD_VFS_SERVER, CAP_CLASS_FS, 1u);
    dispatch(0u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), NS_ERR_FULL,
              "nameserver: REGISTER returns FULL when registry is full");
}

static void test_lookup_metadata(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    reset_nameserver();

    register_req(&req, "vfs", 19u, TRACE_PD_VFS_SERVER, CAP_CLASS_FS, 2u);
    dispatch(0u, &req, &rep);

    lookup_req(&req, OP_NS_LOOKUP, "vfs");
    dispatch(0u, &req, &rep);

    ASSERT_EQ(reply_get(&rep, 0), NS_OK, "nameserver: LOOKUP returns OK");
    ASSERT_EQ(reply_get(&rep, 4), 19u, "nameserver: LOOKUP returns channel");
    ASSERT_EQ(reply_get(&rep, 8), TRACE_PD_VFS_SERVER, "nameserver: LOOKUP returns PD id");
    ASSERT_EQ(reply_get(&rep, 12), NS_STATUS_UNKNOWN, "nameserver: LOOKUP returns initial status");
    ASSERT_EQ(reply_get(&rep, 16), CAP_CLASS_FS, "nameserver: LOOKUP returns cap classes");
    ASSERT_EQ(reply_get(&rep, 20), 2u, "nameserver: LOOKUP returns version");
}

static void test_lookup_not_found(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    reset_nameserver();

    lookup_req(&req, OP_NS_LOOKUP, "missing");
    dispatch(0u, &req, &rep);

    ASSERT_EQ(reply_get(&rep, 0), NS_ERR_NOT_FOUND,
              "nameserver: LOOKUP unknown service returns NOT_FOUND");
}

static void test_update_status(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    reset_nameserver();

    register_req(&req, "vfs", 19u, TRACE_PD_VFS_SERVER, CAP_CLASS_FS, 2u);
    dispatch(0u, &req, &rep);

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, OP_NS_UPDATE_STATUS);
    req_set(&req, 4, 19u);
    req_set(&req, 8, NS_STATUS_READY);
    dispatch(0u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), NS_OK, "nameserver: UPDATE_STATUS returns OK");

    lookup_req(&req, OP_NS_LOOKUP, "vfs");
    dispatch(0u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 12), NS_STATUS_READY,
              "nameserver: LOOKUP observes updated status");
}

static void test_update_status_not_found(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    reset_nameserver();

    req_set(&req, 0, OP_NS_UPDATE_STATUS);
    req_set(&req, 4, 404u);
    req_set(&req, 8, NS_STATUS_READY);
    dispatch(0u, &req, &rep);

    ASSERT_EQ(reply_get(&rep, 0), NS_ERR_NOT_FOUND,
              "nameserver: UPDATE_STATUS unknown channel returns NOT_FOUND");
}

static void test_gated_lookup(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    reset_nameserver();

    register_req(&req, "vfs", 19u, TRACE_PD_VFS_SERVER, CAP_CLASS_FS, 2u);
    dispatch(0u, &req, &rep);

    lookup_req(&req, OP_NS_LOOKUP_GATED, "vfs");
    dispatch(0u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), NS_ERR_FORBIDDEN,
              "nameserver: gated LOOKUP with empty badge is forbidden");

    lookup_req(&req, OP_NS_LOOKUP_GATED, "vfs");
    dispatch((CAP_CLASS_FS << 16) | 3u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), NS_OK,
              "nameserver: gated LOOKUP with matching cap class succeeds");
}

static void test_gated_lookup_not_found(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    reset_nameserver();

    lookup_req(&req, OP_NS_LOOKUP_GATED, "missing");
    dispatch((CAP_CLASS_FS << 16) | 3u, &req, &rep);

    ASSERT_EQ(reply_get(&rep, 0), NS_ERR_NOT_FOUND,
              "nameserver: gated LOOKUP unknown service returns NOT_FOUND");
}

static void test_list_empty_shmem(void)
{
    static uint8_t shmem[8192];
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    reset_nameserver();
    memset(shmem, 0xA5, sizeof(shmem));
    ns_registry_shmem_vaddr = (uintptr_t)shmem;

    req_set(&req, 0, OP_NS_LIST);
    dispatch(0u, &req, &rep);

    ns_list_header_t *hdr = (ns_list_header_t *)shmem;
    ASSERT_EQ(reply_get(&rep, 0), NS_OK, "nameserver: LIST empty registry returns OK");
    ASSERT_EQ(reply_get(&rep, 4), 0u, "nameserver: LIST empty registry reply count is 0");
    ASSERT_EQ(hdr->count, 0u, "nameserver: LIST empty registry writes shmem count 0");
}

static void test_list_shmem(void)
{
    static uint8_t shmem[8192];
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    reset_nameserver();
    memset(shmem, 0, sizeof(shmem));
    ns_registry_shmem_vaddr = (uintptr_t)shmem;

    register_req(&req, "vfs", 19u, TRACE_PD_VFS_SERVER, CAP_CLASS_FS, 2u);
    dispatch(0u, &req, &rep);

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, OP_NS_LIST);
    dispatch(0u, &req, &rep);

    ns_list_header_t *hdr = (ns_list_header_t *)shmem;
    ASSERT_EQ(reply_get(&rep, 0), NS_OK, "nameserver: LIST returns OK");
    ASSERT_EQ(reply_get(&rep, 4), 1u, "nameserver: LIST reply count is 1");
    ASSERT_EQ(hdr->magic, NS_LIST_MAGIC, "nameserver: LIST writes shmem magic");
    ASSERT_EQ(hdr->count, 1u, "nameserver: LIST writes shmem count");
    ASSERT_TRUE(strcmp(hdr->entries[0].name, "vfs") == 0,
                "nameserver: LIST writes service name");
}

static void test_deregister(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    reset_nameserver();

    register_req(&req, "vfs", 19u, TRACE_PD_VFS_SERVER, CAP_CLASS_FS, 2u);
    dispatch(0u, &req, &rep);

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, OP_NS_DEREGISTER);
    req_set(&req, 4, 19u);
    dispatch(0u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), NS_OK, "nameserver: DEREGISTER returns OK");

    lookup_req(&req, OP_NS_LOOKUP, "vfs");
    dispatch(0u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), NS_ERR_NOT_FOUND,
              "nameserver: LOOKUP after DEREGISTER returns NOT_FOUND");

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, OP_NS_HEALTH);
    dispatch(0u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 4), 0u, "nameserver: DEREGISTER decrements count");
}

static void test_deregister_not_found(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    reset_nameserver();

    req_set(&req, 0, OP_NS_DEREGISTER);
    req_set(&req, 4, 404u);
    dispatch(0u, &req, &rep);

    ASSERT_EQ(reply_get(&rep, 0), NS_ERR_NOT_FOUND,
              "nameserver: DEREGISTER unknown channel returns NOT_FOUND");
}

static void test_deregister_allows_reregister(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    reset_nameserver();

    register_req(&req, "vfs", 19u, TRACE_PD_VFS_SERVER, CAP_CLASS_FS, 1u);
    dispatch(0u, &req, &rep);

    memset(&req, 0, sizeof(req));
    req_set(&req, 0, OP_NS_DEREGISTER);
    req_set(&req, 4, 19u);
    dispatch(0u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), NS_OK,
              "nameserver: DEREGISTER before reregister returns OK");

    register_req(&req, "vfs", 27u, TRACE_PD_VFS_SERVER, CAP_CLASS_FS, 2u);
    dispatch(0u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 0), NS_OK,
              "nameserver: REGISTER same name after deregister returns OK");

    lookup_req(&req, OP_NS_LOOKUP, "vfs");
    dispatch(0u, &req, &rep);
    ASSERT_EQ(reply_get(&rep, 4), 27u,
              "nameserver: LOOKUP after reregister returns new channel");
}

static void test_unknown_opcode(void)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    reset_nameserver();

    req_set(&req, 0, 0xFFFFu);
    dispatch(0u, &req, &rep);

    ASSERT_EQ(reply_get(&rep, 0), NS_ERR_UNKNOWN_OP,
              "nameserver: unknown opcode returns UNKNOWN_OP");
}

int main(void)
{
    TAP_PLAN(39);

    test_health_empty();
    test_lookup_empty_name_bad_args();
    test_register_service();
    test_duplicate_register();
    test_register_table_full();
    test_lookup_metadata();
    test_lookup_not_found();
    test_update_status();
    test_update_status_not_found();
    test_gated_lookup();
    test_gated_lookup_not_found();
    test_list_empty_shmem();
    test_list_shmem();
    test_deregister();
    test_deregister_not_found();
    test_deregister_allows_reregister();
    test_unknown_opcode();

    return tap_exit();
}

#else
int main(void) { return 0; }
#endif
