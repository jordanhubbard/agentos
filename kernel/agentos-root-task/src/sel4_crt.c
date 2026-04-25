/*
 * sel4_crt.c — minimal seL4 C runtime for agentOS service PDs
 *
 * Provides the IPC buffer global that libsel4's inline seL4_SetMR / seL4_GetMR
 * rely on.  Previously supplied by libmicrokit.a; now provided here so PDs
 * can link without the Microkit SDK runtime.
 *
 * The SDK header (sel4/functions.h) declares __sel4_ipc_buffer as
 * 'extern __thread', but for bare-metal single-threaded seL4 PDs a plain
 * global satisfies the linker — identical to how libmicrokit.a exports a
 * 'D' (data section) symbol for this pointer (confirmed via nm).
 */

#include <stdint.h>

/*
 * Forward-declare seL4_IPCBuffer without pulling in sel4/sel4.h.
 * An incomplete-type pointer is sufficient for the definition below.
 * On 64-bit targets: tag(8) + msg[120](960) + userData(8) +
 * caps_or_badges[3](24) + receiveCNode(8) + receiveIndex(8) +
 * receiveDepth(8) = 1024 bytes total; must be aligned to own size.
 */
typedef struct seL4_IPCBuffer_ seL4_IPCBuffer;

#define _SEL4_IPC_BUF_BYTES 1024u

static uint8_t _pd_ipc_buf_storage[_SEL4_IPC_BUF_BYTES]
    __attribute__((aligned(_SEL4_IPC_BUF_BYTES)));

/*
 * Plain global definition — matches libmicrokit.a's 'D' section export.
 * lld resolves bare-metal local-exec TLS references to this symbol at
 * link time, avoiding the need for OS-level TLS setup.
 */
seL4_IPCBuffer *__sel4_ipc_buffer =
    (seL4_IPCBuffer *)_pd_ipc_buf_storage;
