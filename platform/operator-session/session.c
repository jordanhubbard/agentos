#include <platform/operator_session.h>
#include <string.h>

static void reply_text(aos_operator_session_t *s, const char *text)
{
    s->reply_len = (uint32_t)strlen(text);
    memcpy(s->reply, text, s->reply_len);
}

static void finish_line(aos_operator_session_t *s, const aos_inspect_snapshot_t *snap)
{
    if (s->overlong) reply_text(s, "error line-too-long\n");
    else if (s->invalid) reply_text(s, "error invalid-line\n");
    else {
        if (s->line_len && s->line[s->line_len - 1] == '\r') s->line_len--;
        s->line[s->line_len] = 0;
        if (strcmp(s->line, "inspect.snapshot")) reply_text(s, "error unknown-command\n");
        else {
            /* Reserve a bounded decimal length header, then move the result
             * down. memmove is required because these private buffers overlap. */
            const uint32_t reserve = 24;
            int n = aos_inspect_format(snap, (char *)s->reply + reserve,
                                       sizeof(s->reply) - reserve);
            if (n < 0 || !(snap->flags & AOS_INSPECT_FLAG_BOOT))
                reply_text(s, "error unavailable\n");
            else {
                char digits[10]; uint32_t value = (uint32_t)n, count = 0, h = 3;
                do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
                memcpy(s->reply, "ok ", 3);
                while (count) s->reply[h++] = (uint8_t)digits[--count];
                s->reply[h++] = '\n';
                memmove(s->reply + h, s->reply + reserve, (size_t)n);
                s->reply_len = h + (uint32_t)n;
            }
        }
    }
    s->line_len = s->invalid = s->overlong = 0;
}

int aos_operator_session_pump(aos_operator_session_t *s,
    const aos_inspect_snapshot_t *snap, const aos_serial_queue_handle_t *input,
    const aos_serial_queue_handle_t *output)
{
    if (!s || !snap || !input || !output) return -1;
    int progress = 0;
    for (uint32_t budget = 0; budget <= AOS_OPERATOR_PUMP_BUDGET; budget++) {
        if (s->reply_len) {
            aos_serial_pump_status_t status = aos_serial_queue_write(output, s->reply, s->reply_len);
            if (status == AOS_SERIAL_PUMP_INVALID) return -1;
            if (status == AOS_SERIAL_PUMP_FULL) return progress;
            s->reply_len = 0;
            progress = 1;
        }
        if (budget == AOS_OPERATOR_PUMP_BUDGET) break;
        uint8_t c; uint32_t n = 0;
        if (aos_serial_queue_read(input, &c, 1, &n) != AOS_SERIAL_PUMP_OK) return -1;
        if (!n) break;
        progress = 1;
        if (c == '\n') { finish_line(s, snap); continue; }
        if (s->line_len == AOS_OPERATOR_LINE_MAX) { s->overlong = 1; continue; }
        if (c != '\r' && (c < 32 || c > 126)) s->invalid = 1;
        /* CR is accepted only as the final byte preceding LF. */
        if (s->line_len && s->line[s->line_len - 1] == '\r') s->invalid = 1;
        s->line[s->line_len++] = (char)c;
    }
    return progress;
}
