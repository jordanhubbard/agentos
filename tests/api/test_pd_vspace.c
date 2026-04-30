/*
 * test_pd_vspace.c — host ABI tests for PD VSpace entry points
 */

#ifdef AGENTOS_TEST_HOST

#include "framework.h"
#include "pd_vspace.h"

#include "../../kernel/agentos-root-task/src/pd_vspace.c"

static void test_create_host_stub(void)
{
    pd_vspace_result_t r = pd_vspace_create(10u, 11u);

    ASSERT_EQ(r.error, seL4_IllegalOperation,
              "pd_vspace_create: host stub returns IllegalOperation");
    ASSERT_EQ(r.vspace_cap, seL4_CapNull,
              "pd_vspace_create: host stub has null vspace cap");
    ASSERT_EQ(r.entry_point, 0u,
              "pd_vspace_create: host stub has zero entry point");
    ASSERT_EQ(r.ipc_buf_cap, seL4_CapNull,
              "pd_vspace_create: host stub has null IPC buffer cap");
}

static void test_load_elf_host_stub_null(void)
{
    pd_vspace_result_t r = pd_vspace_load_elf(10u, (const void *)0, 0u, 4096u);

    ASSERT_EQ(r.error, seL4_IllegalOperation,
              "pd_vspace_load_elf: host stub rejects NULL ELF without seL4");
    ASSERT_EQ(r.vspace_cap, seL4_CapNull,
              "pd_vspace_load_elf: failure has null vspace cap");
    ASSERT_EQ(r.stack_top, 0u,
              "pd_vspace_load_elf: failure has zero stack top");
}

static void test_load_elf_host_stub_buffer(void)
{
    uint8_t fake_elf[64];
    memset(fake_elf, 0, sizeof(fake_elf));

    pd_vspace_result_t r = pd_vspace_load_elf(10u, fake_elf,
                                              (uint32_t)sizeof(fake_elf), 4096u);

    ASSERT_EQ(r.error, seL4_IllegalOperation,
              "pd_vspace_load_elf: host stub does not attempt real mapping");
    ASSERT_EQ(r.entry_point, 0u,
              "pd_vspace_load_elf: host stub has zero entry point");
    ASSERT_EQ(r.ipc_buf_va, 0u,
              "pd_vspace_load_elf: host stub has zero IPC VA");
}

static void test_map_device_frame_host_stub(void)
{
    ASSERT_EQ(pd_vspace_map_device_frame(1u, 2u, 0x1000u), seL4_IllegalOperation,
              "pd_vspace_map_device_frame: host stub returns IllegalOperation");
}

int main(void)
{
    TAP_PLAN(11);

    test_create_host_stub();
    test_load_elf_host_stub_null();
    test_load_elf_host_stub_buffer();
    test_map_device_frame_host_stub();

    return tap_exit();
}

#else
int main(void) { return 0; }
#endif
