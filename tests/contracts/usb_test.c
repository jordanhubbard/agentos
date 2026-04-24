/*
 * agentOS — usb PD contract test
 * Covered opcodes: MSG_USB_ENUMERATE, MSG_USB_LIST, MSG_USB_OPEN, MSG_USB_CLOSE,
 *   MSG_USB_CONTROL, MSG_USB_BULK_IN, MSG_USB_BULK_OUT, MSG_USB_STATUS
 */
#include "../harness/test_framework.h"
#include "../../kernel/agentos-root-task/include/agentos.h"
#include "../../kernel/agentos-root-task/include/contracts/usb_contract.h"

void run_usb_tests(microkit_channel ch) {
    TEST_SECTION("usb");

    /* STATUS — should always succeed */
    ASSERT_IPC_OK(ch, MSG_USB_STATUS, "usb status");

    /* LIST — should always succeed */
    ASSERT_IPC_OK(ch, MSG_USB_LIST, "usb list");

    /* ENUMERATE — should succeed or return error */
    ASSERT_IPC_OK_OR_ERR(ch, MSG_USB_ENUMERATE, USB_ERR_NO_SLOTS, "usb enumerate");

    /* OPEN — bogus device index */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_USB_OPEN, USB_ERR_BAD_DEV_INDEX, "usb open bogus dev");

    /* CLOSE — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_USB_CLOSE, USB_ERR_BAD_HANDLE, "usb close bogus handle");

    /* CONTROL — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_USB_CONTROL, USB_ERR_BAD_HANDLE, "usb control bogus handle");

    /* BULK_IN — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_USB_BULK_IN, USB_ERR_BAD_HANDLE, "usb bulk in bogus handle");

    /* BULK_OUT — bogus handle */
    seL4_SetMR(1, 0xFFFF);
    ASSERT_IPC_ERR(ch, MSG_USB_BULK_OUT, USB_ERR_BAD_HANDLE, "usb bulk out bogus handle");

    /* Verify constants */
    ASSERT_TRUE(USB_MAX_DEVICES == 32, "usb max devices == 32");
    ASSERT_TRUE(USB_MAX_CLIENTS == 16, "usb max clients == 16");
}
