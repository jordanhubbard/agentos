/* Console bytes must survive the public CLI, including non-text data. */
#define CC_FRAME_TIMEOUT_MS 200
#define main agentctl_main
#include "../../tools/agentctl/agentctl.c"
#undef main
#include <assert.h>
#include <sys/wait.h>

static void stalled_transport(void)
{
    int fds[2];
    uint8_t bytes[32] = {0};
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    struct timespec start, end;
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    assert(!read_full(fds[0], bytes, sizeof(bytes)) && errno == ETIMEDOUT);
    assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    int64_t elapsed = (end.tv_sec - start.tv_sec) * 1000 +
                      (end.tv_nsec - start.tv_nsec) / 1000000;
    assert(elapsed >= 190 && elapsed < 2000);
    /* A prefix without EOF must also expire. */
    assert(write_full(fds[1], bytes, 1));
    assert(!read_full(fds[0], bytes, sizeof(bytes)) && errno == ETIMEDOUT);
    close(fds[0]); close(fds[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    int capacity = 4096;
    assert(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) == 0);
    uint8_t *large = calloc(1, 1024 * 1024);
    assert(large);
    assert(!write_full(fds[0], large, 1024 * 1024) && errno == ETIMEDOUT);
    free(large);
    close(fds[0]); close(fds[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    g_stream_fd = fds[0];
    cc_reply_wire_t reply;
    assert(!cc_call(MSG_CC_INSPECT, 1, 0, 0, NULL, 0, &reply));
    cc_req_wire_t request;
    assert(read_full(fds[1], &request, sizeof(request)));
    assert(request.opcode == MSG_CC_INSPECT && request.mr[0] == 1);
    /* Shutdown follows the single request; no replay or extra frame. */
    assert(recv(fds[1], bytes, sizeof(bytes), 0) == 0);
    close(fds[0]); close(fds[1]);
    g_stream_fd = -1;
}

static void bootstrap_contract(void)
{
    for (unsigned mode = 0; mode < 5; ++mode) {
        int fds[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            close(fds[0]);
            uint8_t byte;
            assert(recv(fds[1], &byte, 1, MSG_DONTWAIT) < 0 && errno == EAGAIN);
            cc_reply_wire_t hello = {.mr = {CC_CONNECTION_MAGIC, 1, 7, 9}};
            if (mode == 1) hello.mr[1] = 2;
            if (mode == 2) hello.mr[2] = hello.mr[3] = 0;
            if (mode == 3) hello.shmem[4095] = 1;
            assert(write_full(fds[1], &hello, sizeof(hello)));
            if (mode == 0 || mode == 4) {
                cc_req_wire_t ack;
                assert(read_full(fds[1], &ack, sizeof(ack)));
                hello.mr[0] = MSG_CC_CONNECTION_SYNC;
                assert(memcmp(&ack, &hello, sizeof(ack)) == 0);
                hello.mr[0] = CC_OK;
                if (mode == 4) hello.mr[2]++;
                assert(write_full(fds[1], &hello, sizeof(hello)));
            }
            close(fds[1]);
            _exit(0);
        }
        close(fds[1]);
        assert(connection_sync(fds[0]) == (mode == 0));
        close(fds[0]);
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
}

static void serve(int fd, unsigned mode)
{
    cc_req_wire_t req;
    assert(read_full(fd, &req, sizeof(req)));
    assert(req.opcode == MSG_CC_LOG_STREAM && req.mr[0] == 7 &&
           req.mr[1] == 19 && req.mr[2] == 0);
    cc_reply_wire_t reply = {.mr = {CC_OK, 4096, 0, 0}};
    for (unsigned i = 0; i < sizeof(reply.shmem); ++i) reply.shmem[i] = (uint8_t)i;
    if (mode == 1) reply.mr[1] = 0;
    if (mode == 2) reply.mr[1] = 4097;
    if (mode == 3) reply.mr[0] = CC_ERR_RELAY_FAULT;
    assert(write_full(fd, &reply, mode == 4 ? 20 : sizeof(reply)));
    close(fd);
}

int main(void)
{
    stalled_transport();
    bootstrap_contract();
    for (unsigned mode = 0; mode < 5; ++mode) {
        int fds[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        fflush(stdout);
        pid_t child = fork();
        assert(child >= 0);
        if (!child) { close(fds[0]); serve(fds[1], mode); _exit(0); }
        close(fds[1]);
        FILE *capture = tmpfile();
        assert(capture);
        int saved = dup(STDOUT_FILENO);
        assert(saved >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
        g_stream_fd = fds[0];
        char *args[] = {"agentctl", "log-stream", "7", "19"};
        assert(agentctl_main(4, args) == (mode < 2 ? 0 : 1));
        assert(fflush(stdout) == 0);
        assert(dup2(saved, STDOUT_FILENO) >= 0);
        close(saved);
        close(fds[0]);
        g_stream_fd = -1;
        rewind(capture);
        char output[8300] = {0};
        size_t length = fread(output, 1, sizeof(output) - 1, capture);
        assert(!ferror(capture) && feof(capture));
        fclose(capture);
        if (mode == 0) {
            const char *prefix = "{\"mr\":[0,4096,0,0],\"data_hex\":\"";
            size_t start = strlen(prefix);
            assert(!strncmp(output, prefix, start));
            const char *hex = "0123456789abcdef";
            for (unsigned i = 0; i < 4096; ++i) {
                assert(output[start + 2*i] == hex[(i & 255) >> 4]);
                assert(output[start + 2*i + 1] == hex[i & 15]);
            }
            assert(!strcmp(output + start + 8192, "\"}\n"));
        } else if (mode == 1) {
            assert(!strcmp(output, "{\"mr\":[0,0,0,0],\"data_hex\":\"\"}\n"));
        } else if (mode == 3) {
            char expected[80];
            snprintf(expected, sizeof(expected), "{\"mr\":[%u,4096,0,0]}\n", CC_ERR_RELAY_FAULT);
            assert(!strcmp(output, expected));
        } else {
            assert(length == 0);
        }
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    puts("PASS: console CLI preserves every byte and rejects invalid replies");
}
