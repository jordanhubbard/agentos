#include <platform/operator_session.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static aos_operator_session_t session;
static aos_serial_queue_t iq, oq;
static uint8_t input[4096], output[65536], received[65536];
static aos_serial_queue_handle_t in = {&iq, input, sizeof(input)}, out = {&oq, output, sizeof(output)};
static aos_inspect_snapshot_t snap;
static void send(const void *s, uint32_t n) { CHECK(aos_serial_queue_write(&in, s, n) == 0); }
static void pump(void) { for (unsigned i = 0; i < 40 && aos_operator_session_pump(&session, &snap, &in, &out) > 0; i++) {} }
static uint32_t read_reply(void) {
    uint32_t n; CHECK(aos_serial_queue_read(&out, received, sizeof(received) - 1, &n) == 0);
    received[n] = 0; return n;
}
int main(void)
{
    aos_inspect_view_t view = {.flags = AOS_INSPECT_FLAG_BOOT, .arch = AOS_INSPECT_ARCH_AARCH64,
        .ut_total_bytes = 4096, .thread_count = 1};
    memcpy(view.threads[0].name, "operator_session", 17);
    CHECK(aos_inspect_fill(&snap, &view) == 0);
    send("inspect.", 8); pump(); CHECK(read_reply() == 0);
    send("snapshot\r\n", 10); pump(); uint32_t n = read_reply();
    CHECK(n > 4 && !memcmp(received, "ok ", 3));
    char *end; unsigned long length = strtoul((char *)received + 3, &end, 10);
    CHECK(*end == '\n' && length == (unsigned long)n - (unsigned long)(end + 1 - (char *)received));
    CHECK(strstr(end, "inspect.observation=boot\n") && strstr(end, "operator_session\n"));
    send("bogus\n", 6); pump(); read_reply(); CHECK(!strcmp((char *)received, "error unknown-command\n"));
    const char invalid[] = "inspect\0.snapshot\n";
    send(invalid, sizeof(invalid) - 1); pump(); read_reply(); CHECK(!strcmp((char *)received, "error invalid-line\n"));
    char long_line[1024]; memset(long_line, 'a', sizeof(long_line));
    send(long_line, sizeof(long_line)); pump(); CHECK(read_reply() == 0);
    send("\nbogus\n", 7); pump(); read_reply();
    CHECK(!strcmp((char *)received, "error line-too-long\nerror unknown-command\n"));
    memset(output, 'x', sizeof(output)); oq.head = 0; oq.tail = sizeof(output);
    send("inspect.snapshot\nbogus\n", 23); pump();
    CHECK(session.reply_len && iq.tail - iq.head == 6);
    CHECK(aos_operator_session_pump(&session, &snap, &in, &out) == 0);
    CHECK(read_reply() == sizeof(received) - 1); CHECK(read_reply() == 1);
    pump(); read_reply(); CHECK(!memcmp(received, "ok ", 3));
    CHECK(strstr((char *)received, "error unknown-command\n"));
    iq.tail = iq.head + sizeof(input) + 1;
    CHECK(aos_operator_session_pump(&session, &snap, &in, &out) == -1);
    puts("PASS: fragmented lines, length framing, invalid/oversized recovery, backpressure, corrupt queue");
    return 0;
}
