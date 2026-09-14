#ifndef AOS_OPERATOR_SESSION_H
#define AOS_OPERATOR_SESSION_H
#include <platform/inspect.h>
#include <platform/serial_virt_pump.h>

/* UTF-8's ASCII subset only. Request: "inspect.snapshot\n" (CRLF accepted).
 * Reply: "ok N\n" followed by exactly N structured-text bytes. Errors are
 * "error unknown-command\n", "error invalid-line\n", "error line-too-long\n"
 * or "error unavailable\n". There are no prompts, mutation commands or keys.
 * One pending response applies backpressure; an oversized line is discarded
 * through its newline before accepting another request. No allocation. */
#define AOS_OPERATOR_VERSION 1u
#define AOS_OPERATOR_LINE_MAX 127u
#define AOS_OPERATOR_REPLY_MAX 8192u
#define AOS_OPERATOR_PUMP_BUDGET 256u
typedef struct {
    char line[AOS_OPERATOR_LINE_MAX + 1];
    uint32_t line_len, invalid, overlong;
    uint8_t reply[AOS_OPERATOR_REPLY_MAX];
    uint32_t reply_len;
} aos_operator_session_t;

/* Returns -1 for invalid queues, 0 for quiescence/backpressure, 1 for
 * progress. Call again after yielding when progress consumed the budget;
 * wait on a persistent notification when quiescent. */
int aos_operator_session_pump(aos_operator_session_t *,
    const aos_inspect_snapshot_t *, const aos_serial_queue_handle_t *input,
    const aos_serial_queue_handle_t *output);
#endif
