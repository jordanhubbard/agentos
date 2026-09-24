/*
 * agentOS remoteos_client — protocol tests against a real RemoteOS-SDL.
 *
 * The client is deliberately transport-agnostic. Deterministic mock transport
 * tests assert the wire bytes on every host; when RemoteOS-SDL is available,
 * an additional TCP test proves the same client against the real service.
 *
 * Needs a remoteos-sdl binary. Point REMOTEOS_SDL_BIN at one, or let it find
 * the copy RubyOS vendors. Skips (exit 0) when none is available, so this
 * stays runnable in a checkout that has never built the display service.
 *
 * Build:  cc -o /tmp/test_remoteos_client \
 *             tests/test_remoteos_client.c \
 *             kernel/agentos-root-task/src/remoteos_client.c \
 *             -I kernel/agentos-root-task/include -DAGENTOS_TEST_HOST
 * Run:    /tmp/test_remoteos_client
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include "remoteos_client.h"

static int failures;
static int checks;

static void check(const char *what, bool ok)
{
    checks++;
    if (ok) {
        printf("  PASS  %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        failures++;
    }
}

/* ─── TCP transport ──────────────────────────────────────────────────────── */

static bool tcp_write(void *ctx, const void *buf, uint32_t n)
{
    int fd = *(int *)ctx;
    const uint8_t *p = buf;
    uint32_t sent = 0;
    while (sent < n) {
        ssize_t k = send(fd, p + sent, n - sent, 0);
        if (k < 0) { if (errno == EINTR) continue; return false; }
        if (k == 0) return false;
        sent += (uint32_t)k;
    }
    return true;
}

static bool tcp_read(void *ctx, void *buf, uint32_t n)
{
    int fd = *(int *)ctx;
    uint8_t *p = buf;
    uint32_t got = 0;
    while (got < n) {
        ssize_t k = recv(fd, p + got, n - got, 0);
        if (k < 0) { if (errno == EINTR) continue; return false; }
        if (k == 0) return false;
        got += (uint32_t)k;
    }
    return true;
}

/* A transport that always fails, to prove errors surface rather than hang. */
static bool dead_write(void *ctx, const void *buf, uint32_t n)
{
    (void)ctx; (void)buf; (void)n; return false;
}
static bool dead_read(void *ctx, void *buf, uint32_t n)
{
    (void)ctx; (void)buf; (void)n; return false;
}

typedef struct {
    uint8_t written[8192];
    uint32_t written_len;
    uint8_t reply[1024];
    uint32_t reply_len;
    uint32_t reply_off;
} mock_transport_t;

static bool mock_write(void *ctx, const void *buf, uint32_t n)
{
    mock_transport_t *mock = ctx;
    if (n > sizeof mock->written - mock->written_len) return false;
    memcpy(mock->written + mock->written_len, buf, n);
    mock->written_len += n;
    return true;
}

static bool mock_read(void *ctx, void *buf, uint32_t n)
{
    mock_transport_t *mock = ctx;
    if (n > mock->reply_len - mock->reply_off) return false;
    memcpy(buf, mock->reply + mock->reply_off, n);
    mock->reply_off += n;
    return true;
}

static void mock_reply(mock_transport_t *mock, const char *json)
{
    uint32_t n = (uint32_t)strlen(json);
    memset(mock, 0, sizeof *mock);
    mock->reply[0] = (uint8_t)(n >> 24);
    mock->reply[1] = (uint8_t)(n >> 16);
    mock->reply[2] = (uint8_t)(n >> 8);
    mock->reply[3] = (uint8_t)n;
    memcpy(mock->reply + 4, json, n);
    mock->reply_len = n + 4;
}

static const char *mock_envelope(mock_transport_t *mock, uint32_t *length)
{
    if (mock->written_len < 4) return NULL;
    *length = ((uint32_t)mock->written[0] << 24)
            | ((uint32_t)mock->written[1] << 16)
            | ((uint32_t)mock->written[2] << 8)
            | mock->written[3];
    if (*length > mock->written_len - 4) return NULL;
    return (const char *)mock->written + 4;
}

static bool bytes_contain(const char *bytes, uint32_t length, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len > length) return false;
    for (uint32_t i = 0; i <= length - needle_len; i++)
        if (memcmp(bytes + i, needle, needle_len) == 0) return true;
    return false;
}

/* ─── Service under test ─────────────────────────────────────────────────── */

static const char *find_service(void)
{
    const char *env = getenv("REMOTEOS_SDL_BIN");
    static const char *candidates[] = {
        NULL,
        "../RubyOS/services/remoteos-sdl/remoteos-sdl",
        "../rubyos/services/remoteos-sdl/remoteos-sdl",
        "services/remoteos-sdl/remoteos-sdl",
        NULL,
    };
    candidates[0] = env;
    for (int i = 0; i < 4; i++) {
        if (!candidates[i]) continue;
        struct stat sb;
        if (stat(candidates[i], &sb) == 0 && (sb.st_mode & S_IXUSR))
            return candidates[i];
    }
    return NULL;
}

static int free_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    socklen_t len = sizeof a;
    if (getsockname(fd, (struct sockaddr *)&a, &len) < 0) { close(fd); return -1; }
    int port = ntohs(a.sin_port);
    close(fd);
    return port;
}

static int connect_retry(int port)
{
    for (int attempt = 0; attempt < 400; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)port);
        if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            return fd;
        }
        close(fd);
        usleep(25000);
    }
    return -1;
}

/*
 * debug.capture is not part of the client API -- a PD has no business writing
 * host files -- but the test needs it to prove pixels really landed rather
 * than merely that the service said ok. Framing it by hand here also checks
 * the wire format independently of the writer under test.
 */
static bool capture_bmp(int fd, const char *path)
{
    char env[512];
    int n = snprintf(env, sizeof env,
                     "{\"v\":2,\"id\":9001,\"op\":\"debug.capture\","
                     "\"params\":{\"path\":\"%s\"}}", path);
    if (n <= 0 || (size_t)n >= sizeof env) return false;

    uint8_t hdr[4] = { 0, 0, (uint8_t)(n >> 8), (uint8_t)n };
    if (!tcp_write(&fd, hdr, 4)) return false;
    if (!tcp_write(&fd, env, (uint32_t)n)) return false;

    uint8_t rhdr[4];
    if (!tcp_read(&fd, rhdr, 4)) return false;
    uint32_t len = ((uint32_t)rhdr[0] << 24) | ((uint32_t)rhdr[1] << 16)
                 | ((uint32_t)rhdr[2] << 8) | rhdr[3];
    char reply[1024];
    if (len >= sizeof reply) return false;
    if (!tcp_read(&fd, reply, len)) return false;
    reply[len] = 0;
    return strstr(reply, "\"ok\":true") != NULL;
}

/* Read a 24/32-bit BMP pixel. Rows are bottom-up. */
static bool bmp_pixel(const char *path, int x, int y, uint32_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[54];
    if (fread(h, 1, 54, f) != 54) { fclose(f); return false; }
    uint32_t off = (uint32_t)h[10] | ((uint32_t)h[11] << 8)
                 | ((uint32_t)h[12] << 16) | ((uint32_t)h[13] << 24);
    int32_t w = (int32_t)((uint32_t)h[18] | ((uint32_t)h[19] << 8)
              | ((uint32_t)h[20] << 16) | ((uint32_t)h[21] << 24));
    int32_t hh = (int32_t)((uint32_t)h[22] | ((uint32_t)h[23] << 8)
               | ((uint32_t)h[24] << 16) | ((uint32_t)h[25] << 24));
    uint16_t bpp = (uint16_t)((uint32_t)h[28] | ((uint32_t)h[29] << 8));
    if (w <= 0 || hh == 0 || (bpp != 24 && bpp != 32)) { fclose(f); return false; }
    int32_t height = hh < 0 ? -hh : hh;
    if (x < 0 || y < 0 || x >= w || y >= height) { fclose(f); return false; }

    uint32_t bytes = bpp / 8u;
    uint32_t stride = (((uint32_t)w * bpp + 31u) / 32u) * 4u;
    int32_t row = (hh > 0) ? (height - 1 - y) : y;
    if (fseek(f, (long)(off + (uint32_t)row * stride + (uint32_t)x * bytes),
              SEEK_SET) != 0) { fclose(f); return false; }
    uint8_t px[4] = {0};
    if (fread(px, 1, bytes, f) != bytes) { fclose(f); return false; }
    fclose(f);
    *out = ((uint32_t)px[2] << 16) | ((uint32_t)px[1] << 8) | px[0];
    return true;
}

/* ─── Tests ──────────────────────────────────────────────────────────────── */

static void test_argument_guards(void)
{
    printf("\nArgument guards\n");
    remoteos_client_t c;
    uint8_t buf[REMOTEOS_MIN_BUFFER];
    remoteos_transport_t dead = { NULL, dead_write, dead_read };

    check("init rejects a NULL transport",
          remoteos_init(&c, NULL, buf, sizeof buf) == REMOTEOS_ERR_ARG);
    check("init rejects a buffer that cannot hold an envelope",
          remoteos_init(&c, &dead, buf, 8) == REMOTEOS_ERR_ARG);
    check("init accepts a usable configuration",
          remoteos_init(&c, &dead, buf, sizeof buf) == REMOTEOS_OK);
    check("display.open rejects a zero dimension",
          remoteos_display_open(&c, 0, 240, "x", NULL, NULL, NULL)
              == REMOTEOS_ERR_ARG);
    check("upload rejects an empty payload",
          remoteos_surface_upload(&c, 1, "x", 0) == REMOTEOS_ERR_ARG);
}

static void test_transport_failure(void)
{
    printf("\nTransport failure\n");
    remoteos_client_t c;
    uint8_t buf[REMOTEOS_MIN_BUFFER];
    remoteos_transport_t dead = { NULL, dead_write, dead_read };
    remoteos_init(&c, &dead, buf, sizeof buf);
    /* A dead link must be reported, not retried forever or silently ignored. */
    check("hello reports a dead transport",
          remoteos_hello(&c, "agentos-test") == REMOTEOS_ERR_TRANSPORT);
}

static void test_buffer_overflow(void)
{
    printf("\nEnvelope overflow\n");
    remoteos_client_t c;
    uint8_t buf[REMOTEOS_MIN_BUFFER];
    remoteos_transport_t dead = { NULL, dead_write, dead_read };
    remoteos_init(&c, &dead, buf, REMOTEOS_MIN_BUFFER);

    /* A title far longer than the buffer must overflow cleanly, before any
     * bytes reach the wire -- a truncated envelope would desynchronise the
     * stream permanently. */
    char big[REMOTEOS_MIN_BUFFER * 2];
    memset(big, 'A', sizeof big - 1);
    big[sizeof big - 1] = 0;
    check("an oversized title overflows instead of truncating",
          remoteos_display_open(&c, 320, 240, big, NULL, NULL, NULL)
              == REMOTEOS_ERR_OVERFLOW);
}

static void test_wire_protocol(void)
{
    printf("\nDeterministic wire protocol\n");
    mock_transport_t mock;
    remoteos_transport_t transport = { &mock, mock_write, mock_read };
    remoteos_client_t client;
    uint8_t buffer[REMOTEOS_MIN_BUFFER];
    uint32_t length = 0;
    const char *envelope;

    mock_reply(&mock, "{\"v\":2,\"id\":1,\"ok\":true,\"result\":{}}");
    check("mock client initializes",
          remoteos_init(&client, &transport, buffer, sizeof buffer) == REMOTEOS_OK);
    check("hello succeeds with a deterministic reply",
          remoteos_hello(&client, "agent\\\"os") == REMOTEOS_OK);
    envelope = mock_envelope(&mock, &length);
    check("hello framing and escaping are exact",
          envelope && length == strlen("{\"v\":2,\"id\":1,\"op\":\"hello\",\"params\":{\"protocol\":2,\"client\":\"agent\\\\\\\"os\"}}") &&
          memcmp(envelope,
                 "{\"v\":2,\"id\":1,\"op\":\"hello\",\"params\":{\"protocol\":2,\"client\":\"agent\\\\\\\"os\"}}",
                 length) == 0);

    mock_reply(&mock, "{\"v\":2,\"id\":2,\"ok\":true,\"result\":{\"w\":800,\"h\":600,\"fb_handle\":9}}");
    uint32_t width = 0, height = 0, handle = 0;
    check("display.open adopts returned geometry and handle",
          remoteos_display_open(&client, 640, 480, "agentOS", &width, &height, &handle) == REMOTEOS_OK &&
          width == 800 && height == 600 && handle == 9);

    mock_reply(&mock, "{\"v\":2,\"id\":3,\"ok\":true,\"result\":{}}");
    check("fill_rect accepts signed coordinates",
          remoteos_fill_rect(&client, 9, -2, 3, 4, 5, 0x112233) == REMOTEOS_OK);
    envelope = mock_envelope(&mock, &length);
    check("fill_rect encodes its rectangle and color",
          envelope && bytes_contain(envelope, length, "\"x\":-2") &&
          bytes_contain(envelope, length, "\"rgb\":1122867"));

    mock_reply(&mock, "{\"v\":2,\"id\":4,\"ok\":true,\"result\":{}}");
    const uint8_t pixels[] = { 1, 2, 3, 4 };
    check("surface.upload succeeds",
          remoteos_surface_upload(&client, 9, pixels, sizeof pixels) == REMOTEOS_OK);
    envelope = mock_envelope(&mock, &length);
    check("surface.upload appends the exact binary trailer",
          envelope && mock.written_len == length + 4 + sizeof pixels &&
          memcmp(mock.written + 4 + length, pixels, sizeof pixels) == 0);

    mock_reply(&mock, "{\"v\":2,\"id\":5,\"ok\":false,\"code\":42}");
    check("remote errors preserve their service code",
          remoteos_frame_commit(&client) == REMOTEOS_ERR_REMOTE &&
          remoteos_remote_code(&client) == 42);

    mock_reply(&mock, "{\"v\":2,\"id\":99,\"ok\":true,\"result\":{}}");
    check("a mismatched reply id fails closed",
          remoteos_display_close(&client) == REMOTEOS_ERR_PROTOCOL);
}

static int test_live_service(const char *bin)
{
    printf("\nLive service: %s\n", bin);

    int port = free_port();
    if (port < 0) { printf("  SKIP  no free port\n"); return 0; }

    char addr[64];
    snprintf(addr, sizeof addr, "127.0.0.1:%d", port);

    pid_t pid = fork();
    if (pid < 0) { printf("  SKIP  fork failed\n"); return 0; }
    if (pid == 0) {
        setenv("REMOTEOS_SDL_MODE", "headless", 1);
        setenv("SDL_VIDEODRIVER", "dummy", 1);
        setenv("SDL_AUDIODRIVER", "dummy", 1);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
        execl(bin, bin, "--listen-tcp", addr, (char *)NULL);
        _exit(127);
    }

    int fd = connect_retry(port);
    if (fd < 0) {
        printf("  FAIL  could not connect to the service\n");
        failures++; checks++;
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        return 1;
    }

    remoteos_client_t c;
    uint8_t buf[4096];
    remoteos_transport_t tp = { &fd, tcp_write, tcp_read };
    remoteos_init(&c, &tp, buf, sizeof buf);

    check("hello negotiates protocol v2",
          remoteos_hello(&c, "agentos-remoteos-test/1") == REMOTEOS_OK);

    uint32_t w = 0, h = 0, fb = 0;
    remoteos_status_t st =
        remoteos_display_open(&c, 320, 240, "agentOS relay", &w, &h, &fb);
    check("display.open succeeds", st == REMOTEOS_OK);
    check("display.open reports the framebuffer size", w == 320 && h == 240);
    check("display.open yields a framebuffer handle", fb != 0);

    check("surface.fill_rect paints the background",
          remoteos_fill_rect(&c, fb, 0, 0, 320, 240, 0x101820) == REMOTEOS_OK);
    check("surface.fill_rect paints a marker",
          remoteos_fill_rect(&c, fb, 40, 30, 80, 60, 0x22cc88) == REMOTEOS_OK);
    check("frame.commit presents the frame",
          remoteos_frame_commit(&c) == REMOTEOS_OK);

    /* Pixels, not just acknowledgements. */
    const char *bmp = "/tmp/agentos-remoteos-client.bmp";
    unlink(bmp);
    if (capture_bmp(fd, bmp)) {
        uint32_t px = 0;
        check("the marker rectangle really reached the framebuffer",
              bmp_pixel(bmp, 80, 60, &px) && px == 0x22cc88);
        check("the background really reached the framebuffer",
              bmp_pixel(bmp, 300, 220, &px) && px == 0x101820);
    } else {
        printf("  SKIP  debug.capture unavailable\n");
    }

    /* A bad handle must come back as a service-level error, not a hang. */
    st = remoteos_fill_rect(&c, 4242, 0, 0, 1, 1, 0);
    check("an invalid surface handle is reported as a remote error",
          st == REMOTEOS_ERR_REMOTE);
    check("the service error code is preserved",
          remoteos_remote_code(&c) != 0);

    /* ...and the session keeps working afterwards. */
    check("the connection survives a rejected request",
          remoteos_fill_rect(&c, fb, 0, 0, 4, 4, 0xffffff) == REMOTEOS_OK);

    check("display.close succeeds", remoteos_display_close(&c) == REMOTEOS_OK);
    check("shutdown succeeds", remoteos_shutdown(&c) == REMOTEOS_OK);

    close(fd);
    int status = 0;
    waitpid(pid, &status, 0);
    check("the service exited cleanly",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}

int main(void)
{
    printf("agentOS remoteos_client tests\n");

    test_argument_guards();
    test_transport_failure();
    test_buffer_overflow();
    test_wire_protocol();

    const char *bin = find_service();
    if (bin) {
        test_live_service(bin);
    } else {
        printf("\nLive service\n");
        printf("  SKIP  no remoteos-sdl binary; set REMOTEOS_SDL_BIN\n");
    }

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
