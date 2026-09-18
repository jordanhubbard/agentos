/*
 * agentctl — agentOS Command-and-Control reference consumer
 *
 * Host-side CLI. Connects to cc_pd's Unix socket bridge, sends one binary
 * CC frame, prints structured output, and exits. No interactive UI.
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/time.h>

#include "contracts/cc_contract.h"
#include "contracts/guest_contract.h"
#include <platform/inspect.h>
#include <platform/operator_session.h>
#include <platform/framebuffer_observer.h>
#include <platform/input.h>

#define AGENTCTL_VERSION "0.2.0"
#define DEFAULT_CC_SOCK "build/cc_pd.sock"
#define MY_BADGE 0xA6E70001u
#define CC_WIRE_SHMEM_SIZE 4096u

typedef struct {
    uint32_t opcode;
    uint32_t mr[3];
    uint8_t shmem[CC_WIRE_SHMEM_SIZE];
} cc_req_wire_t;

typedef struct {
    uint32_t mr[4];
    uint8_t shmem[CC_WIRE_SHMEM_SIZE];
} cc_reply_wire_t;

static const char *g_sock_path = DEFAULT_CC_SOCK;
static int g_stream_fd = -1;

static void usage(FILE *out)
{
    fprintf(out,
            "agentctl v%s\n"
            "Usage: agentctl [--socket PATH] [--batch] COMMAND [ARGS...]\n\n"
            "Commands:\n"
            "  inspect\n"
            "  session-inspect\n"
            "  list-guests\n"
            "  guest-status HANDLE\n"
            "  list-devices TYPE [MAX]\n"
            "  device-status TYPE HANDLE\n"
            "  polecats | list-polecats\n"
            "  log-stream SLOT PD_ID\n"
            "  fb-attach GUEST_HANDLE FB_HANDLE\n"
            "  frame-capture GUEST_HANDLE OUTPUT.ppm\n"
            "  input-batch GUEST_HANDLE keyboard|pointer TYPE CODE VALUE [TYPE CODE VALUE ...]\n"
            "  send-input GUEST_HANDLE KEYCODE\n"
            "  suspend GUEST_HANDLE\n"
            "  resume GUEST_HANDLE\n"
            "  destroy GUEST_HANDLE [REASON]\n"
            "  snapshot GUEST_HANDLE\n"
            "  restore GUEST_HANDLE SNAP_LO SNAP_HI\n"
            "  trace-start [FLAGS]\n"
            "  trace-stop\n"
            "  trace-query\n"
            "  trace-dump [MAX_EVENTS]\n"
            "  connect\n"
            "  status SESSION_ID\n"
            "  raw OPCODE [MR1 [MR2 [MR3]]]\n\n"
            "Socket defaults to $CC_PD_SOCK, then %s.\n",
            AGENTCTL_VERSION, DEFAULT_CC_SOCK);
}

static uint32_t parse_u32(const char *s, const char *name)
{
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || v > UINT32_MAX) {
        fprintf(stderr, "agentctl: invalid %s: %s\n", name, s);
        exit(2);
    }
    return (uint32_t)v;
}

static int connect_cc(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("agentctl: socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(g_sock_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "agentctl: socket path too long: %s\n", g_sock_path);
        close(fd);
        return -1;
    }
    strcpy(addr.sun_path, g_sock_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "agentctl: cannot connect to %s: %s\n",
                g_sock_path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static bool write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += (size_t)w;
        n -= (size_t)w;
    }
    return true;
}

static bool read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += (size_t)r;
        n -= (size_t)r;
    }
    return true;
}

static bool cc_call(uint32_t opcode, uint32_t mr1, uint32_t mr2, uint32_t mr3,
                    const void *shmem, size_t shmem_len, cc_reply_wire_t *reply)
{
    cc_req_wire_t req;
    memset(&req, 0, sizeof(req));
    memset(reply, 0, sizeof(*reply));
    req.opcode = opcode;
    req.mr[0] = mr1;
    req.mr[1] = mr2;
    req.mr[2] = mr3;
    if (shmem && shmem_len > 0) {
        if (shmem_len > sizeof(req.shmem)) shmem_len = sizeof(req.shmem);
        memcpy(req.shmem, shmem, shmem_len);
    }

    int fd = g_stream_fd >= 0 ? g_stream_fd : connect_cc();
    if (fd < 0) return false;
    bool ok = write_full(fd, &req, sizeof(req)) &&
              read_full(fd, reply, sizeof(*reply));
    if (g_stream_fd < 0) close(fd);
    if (!ok) {
        fprintf(stderr, "agentctl: CC frame I/O failed\n");
    }
    return ok;
}

static void print_raw_reply(const cc_reply_wire_t *r)
{
    printf("{\"mr\":[%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "]}\n",
           r->mr[0], r->mr[1], r->mr[2], r->mr[3]);
}

static bool frame_call(uint32_t handle, const aos_fb_observer_request_t *query,
                       aos_fb_observer_response_t *response, uint8_t *pixels)
{
    cc_reply_wire_t reply;
    if (!cc_call(MSG_CC_FRAME_CAPTURE, handle, 0, 0, query, sizeof(*query), &reply))
        return false;
    memcpy(response, reply.shmem, sizeof(*response));
    if (reply.mr[0] != CC_OK || reply.mr[1] < sizeof(*response) ||
        reply.mr[1] > sizeof(reply.shmem) || reply.mr[3] != AOS_FB_OBSERVER_VERSION ||
        response->version != AOS_FB_OBSERVER_VERSION || response->id ||
        response->status != AOS_FB_OBSERVER_OK || reply.mr[2] != response->status ||
        response->length != reply.mr[1] - sizeof(*response) ||
        response->length != (query->operation == AOS_FB_CAPTURE_READ ? query->length : 0)) {
        fprintf(stderr, "agentctl: frame capture failed (CC=%u, observer=%u)\n",
                reply.mr[0], response->status);
        return false;
    }
    if (response->length) memcpy(pixels, reply.shmem + sizeof(*response), response->length);
    return true;
}

static int cmd_frame_capture(uint32_t handle, const char *path)
{
    g_stream_fd = connect_cc();
    if (g_stream_fd < 0) return 1;
    aos_fb_observer_request_t query = {.version=AOS_FB_OBSERVER_VERSION,
        .operation=AOS_FB_CAPTURE};
    aos_fb_observer_response_t snapshot, response;
    uint8_t *pixels = NULL;
    int result = 1;
    if (!frame_call(handle, &query, &snapshot, NULL)) goto done;
    query.cookie = snapshot.cookie;
    if (!snapshot.cookie || !snapshot.sequence || !snapshot.width || !snapshot.height ||
        snapshot.width > AOS_FB_MAX_WIDTH || snapshot.height > AOS_FB_MAX_HEIGHT) goto release;
    uint32_t bytes = snapshot.width * snapshot.height * AOS_FB_PIXEL_BYTES;
    pixels = malloc(bytes);
    if (!pixels) goto release;
    query.operation = AOS_FB_CAPTURE_READ;
    for (query.offset = 0; query.offset < bytes; query.offset += query.length) {
        query.length = bytes - query.offset;
        if (query.length > CC_WIRE_SHMEM_SIZE - sizeof(response))
            query.length = CC_WIRE_SHMEM_SIZE - sizeof(response);
        if (!frame_call(0, &query, &response, pixels + query.offset) ||
            response.cookie != snapshot.cookie || response.sequence != snapshot.sequence ||
            response.width != snapshot.width || response.height != snapshot.height) goto release;
    }
    /* PPM is a portable raster artifact, not an in-repository viewer. Refuse
     * to overwrite an existing file; never leave a partial capture on error. */
    FILE *file = fopen(path, "wx");
    if (!file) { perror("agentctl: capture output"); goto release; }
    bool written = fprintf(file, "P6\n%u %u\n255\n", snapshot.width, snapshot.height) > 0;
    for (uint32_t offset = 0; written && offset < bytes; offset += 4) {
        const uint8_t rgb[] = {pixels[offset+2], pixels[offset+1], pixels[offset]};
        written = fwrite(rgb, 1, sizeof(rgb), file) == sizeof(rgb);
    }
    if (fclose(file) != 0) written = false;
    if (!written) { unlink(path); goto release; }
    printf("{\"width\":%u,\"height\":%u,\"sequence\":%" PRIu64 ",\"bytes\":%u}\n",
           snapshot.width, snapshot.height, snapshot.sequence, bytes);
    result = 0;
release:
    query.operation = AOS_FB_CAPTURE_RELEASE;
    query.offset = query.length = 0;
    if (query.cookie && !frame_call(0, &query, &response, NULL)) result = 1;
done:
    free(pixels);
    close(g_stream_fd);
    g_stream_fd = -1;
    return result;
}

static int cmd_inspect(void)
{
    cc_reply_wire_t r;
    aos_inspect_snapshot_t snap;
    char report[8192];
    if (!cc_call(MSG_CC_INSPECT, AOS_INSPECT_VERSION, 0, 0, NULL, 0, &r)) return 1;
    if (r.mr[0] != CC_OK || r.mr[1] != sizeof(snap) || r.mr[3] != AOS_INSPECT_VERSION) {
        fprintf(stderr, "agentctl: invalid or unavailable inspect reply\n");
        return 1;
    }
    memcpy(&snap, r.shmem, sizeof(snap));
    if (snap.flags != r.mr[2] || !(snap.flags & AOS_INSPECT_FLAG_BOOT) ||
        aos_inspect_format(&snap, report, sizeof(report)) < 0) {
        fprintf(stderr, "agentctl: invalid inspect snapshot\n");
        return 1;
    }
    fputs(report, stdout);
    return 0;
}

static int cmd_session_inspect(void)
{
    static const char command[] = "inspect.snapshot\n";
    uint8_t bytes[AOS_OPERATOR_REPLY_MAX + 1];
    size_t used = 0;
    cc_reply_wire_t reply;
    int result = 1;
    g_stream_fd = connect_cc();
    if (g_stream_fd < 0) return 1;
    struct timeval timeout = {5, 0};
    if (setsockopt(g_stream_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(g_stream_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) goto done;
    if (!cc_call(MSG_CC_OPERATOR_WRITE, AOS_OPERATOR_VERSION, sizeof(command) - 1, 0,
                 command, sizeof(command) - 1, &reply) || reply.mr[0] != CC_OK ||
        reply.mr[1] != sizeof(command) - 1) goto done;
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) goto done;
    for (;;) {
        if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec - start.tv_sec >= 30) break;
        uint32_t max = sizeof(bytes) - 1 - used;
        if (max > CC_WIRE_SHMEM_SIZE) max = CC_WIRE_SHMEM_SIZE;
        if (!max || !cc_call(MSG_CC_OPERATOR_READ, AOS_OPERATOR_VERSION, max, 0,
                            NULL, 0, &reply) || reply.mr[0] != CC_OK || reply.mr[1] > max) break;
        memcpy(bytes + used, reply.shmem, reply.mr[1]); used += reply.mr[1]; bytes[used] = 0;
        uint8_t *newline = memchr(bytes, '\n', used);
        if (newline) {
            if (used < 4 || memcmp(bytes, "ok ", 3) || bytes[3] < '0' || bytes[3] > '9') break;
            char *end; errno = 0;
            unsigned long count = strtoul((char *)bytes + 3, &end, 10);
            size_t header = (size_t)(newline - bytes) + 1;
            if (errno || end != (char *)newline || !count || count > sizeof(bytes) - 1 - header) break;
            if (used > header + count) break;
            if (used == header + count) {
                bool valid = true;
                for (size_t i = header; i < used; i++)
                    if (bytes[i] != '\n' && (bytes[i] < 32 || bytes[i] > 126)) valid = false;
                if (!valid) break;
                result = fwrite(bytes + header, 1, count, stdout) == count ? 0 : 1;
                break;
            }
        }
        if (!reply.mr[1]) (void)poll(NULL, 0, 10);
    }
done:
    close(g_stream_fd); g_stream_fd = -1;
    if (result) fprintf(stderr, "agentctl: incomplete or invalid operator session response\n");
    return result;
}

static int cmd_connect(void)
{
    cc_reply_wire_t r;
    if (!cc_call(MSG_CC_CONNECT, MY_BADGE, CC_CONNECT_FLAG_BINARY, 0,
                 NULL, 0, &r)) return 1;
    printf("{\"ok\":%" PRIu32 ",\"session_id\":%" PRIu32 "}\n",
           r.mr[0], r.mr[1]);
    return r.mr[0] == CC_OK ? 0 : 1;
}

static int cmd_status(int argc, char **argv)
{
    if (argc < 1) return 2;
    cc_reply_wire_t r;
    uint32_t sid = parse_u32(argv[0], "session_id");
    if (!cc_call(MSG_CC_STATUS, sid, 0, 0, NULL, 0, &r)) return 1;
    printf("{\"ok\":%" PRIu32 ",\"state\":%" PRIu32
           ",\"pending_responses\":%" PRIu32
           ",\"ticks_since_active\":%" PRIu32 "}\n",
           r.mr[0], r.mr[1], r.mr[2], r.mr[3]);
    return r.mr[0] == CC_OK ? 0 : 1;
}

static int cmd_list_guests(void)
{
    cc_reply_wire_t r;
    if (!cc_call(MSG_CC_LIST_GUESTS, 64, 0, 0, NULL, 0, &r)) return 1;
    uint32_t count = r.mr[0];
    size_t max = sizeof(r.shmem) / sizeof(cc_guest_info_t);
    if (count > max) count = (uint32_t)max;
    cc_guest_info_t *g = (cc_guest_info_t *)r.shmem;
    printf("{\"count\":%" PRIu32 ",\"guests\":[", count);
    for (uint32_t i = 0; i < count; i++) {
        if (i) printf(",");
        printf("{\"guest_handle\":%" PRIu32 ",\"state\":%" PRIu32
               ",\"os_type\":%" PRIu32 ",\"arch\":%" PRIu32 "}",
               g[i].guest_handle, g[i].state, g[i].os_type, g[i].arch);
    }
    printf("]}\n");
    return 0;
}

static int cmd_guest_status(int argc, char **argv)
{
    if (argc < 1) return 2;
    cc_reply_wire_t r;
    uint32_t handle = parse_u32(argv[0], "guest_handle");
    if (!cc_call(MSG_CC_GUEST_STATUS, handle, 0, 0, NULL, 0, &r)) return 1;
    printf("{\"ok\":%" PRIu32, r.mr[0]);
    if (r.mr[0] == CC_OK) {
        cc_guest_status_t *s = (cc_guest_status_t *)r.shmem;
        printf(",\"guest_handle\":%" PRIu32 ",\"state\":%" PRIu32
               ",\"os_type\":%" PRIu32 ",\"arch\":%" PRIu32
               ",\"device_flags\":%" PRIu32,
               s->guest_handle, s->state, s->os_type, s->arch, s->device_flags);
    }
    printf("}\n");
    return r.mr[0] == CC_OK ? 0 : 1;
}

static int cmd_list_devices(int argc, char **argv)
{
    if (argc < 1) return 2;
    uint32_t dev_type = parse_u32(argv[0], "dev_type");
    uint32_t max_entries = argc > 1 ? parse_u32(argv[1], "max") : 64u;
    cc_reply_wire_t r;
    if (!cc_call(MSG_CC_LIST_DEVICES, dev_type, max_entries, 0, NULL, 0, &r)) {
        return 1;
    }
    uint32_t count = r.mr[0];
    size_t max = sizeof(r.shmem) / sizeof(cc_device_info_t);
    if (count > max) count = (uint32_t)max;
    cc_device_info_t *d = (cc_device_info_t *)r.shmem;
    printf("{\"count\":%" PRIu32 ",\"devices\":[", count);
    for (uint32_t i = 0; i < count; i++) {
        if (i) printf(",");
        printf("{\"dev_type\":%" PRIu32 ",\"dev_handle\":%" PRIu32
               ",\"state\":%" PRIu32 "}",
               d[i].dev_type, d[i].dev_handle, d[i].state);
    }
    printf("]}\n");
    return 0;
}

static int cmd_simple(uint32_t opcode, uint32_t mr1, uint32_t mr2, uint32_t mr3)
{
    cc_reply_wire_t r;
    if (!cc_call(opcode, mr1, mr2, mr3, NULL, 0, &r)) return 1;
    print_raw_reply(&r);
    return r.mr[0] == CC_OK ? 0 : 1;
}

static int cmd_input_batch(int argc,char **argv)
{
    if (argc<5 || (argc-2)%3 || (unsigned)(argc-2)/3>=AOS_INPUT_BATCH_EVENTS) return 2;
    aos_input_request_t query={.version=AOS_INPUT_VERSION};
    uint32_t handle=parse_u32(argv[0],"guest_handle");
    if (!strcmp(argv[1],"keyboard")) query.device=AOS_INPUT_KEYBOARD;
    else if (!strcmp(argv[1],"pointer")) query.device=AOS_INPUT_POINTER;
    else return 2;
    for (int i=2;i<argc;i+=3) {
        uint32_t type=parse_u32(argv[i],"event type"),code=parse_u32(argv[i+1],"event code");
        char *end;
        errno=0;
        long long value=strtoll(argv[i+2],&end,0);
        if (errno || end==argv[i+2] || *end || value<INT32_MIN || value>INT32_MAX ||
            type>UINT16_MAX || code>UINT16_MAX || !type) return 2;
        query.events[query.count++]=(aos_input_event_t){(uint16_t)type,(uint16_t)code,(int32_t)value};
    }
    ++query.count; /* zero-initialized final SYN_REPORT completes the batch */
    cc_reply_wire_t reply;
    if (!cc_call(MSG_CC_INPUT_SUBMIT,handle,0,0,&query,sizeof(query),&reply)) return 1;
    aos_input_response_t response;
    memcpy(&response,reply.shmem,sizeof(response));
    if (reply.mr[0]!=CC_OK || reply.mr[1]!=sizeof(response) ||
        reply.mr[3]!=AOS_INPUT_VERSION || response.version!=AOS_INPUT_VERSION ||
        response.id || response.status>AOS_INPUT_WOULD_BLOCK ||
        reply.mr[2]!=response.status ||
        response.accepted!=(response.status==AOS_INPUT_OK ? query.count : 0u)) {
        fprintf(stderr,"agentctl: invalid input response (CC=%u)\n",reply.mr[0]);
        return 1;
    }
    printf("{\"status\":%u,\"accepted\":%u}\n",response.status,response.accepted);
    /* No implicit retries: a transport failure gives no delivery guarantee. */
    return response.status==AOS_INPUT_OK ? 0 : 1;
}

static int cmd_send_input(int argc, char **argv)
{
    if (argc < 2) return 2;
    uint32_t guest = parse_u32(argv[0], "guest_handle");
    cc_input_event_t event;
    memset(&event, 0, sizeof(event));
    event.event_type = CC_INPUT_KEY_DOWN;
    event.keycode = parse_u32(argv[1], "keycode");

    cc_reply_wire_t r;
    if (!cc_call(MSG_CC_SEND_INPUT, guest, 0, 0, &event, sizeof(event), &r)) {
        return 1;
    }
    printf("{\"ok\":%" PRIu32 "}\n", r.mr[0]);
    return r.mr[0] == CC_OK ? 0 : 1;
}

static int cmd_trace_dump(int argc, char **argv)
{
    uint32_t max_events = argc > 0 ? parse_u32(argv[0], "max_events") : 0u;
    cc_reply_wire_t r;
    if (!cc_call(MSG_CC_TRACE_DUMP, max_events, 0, 0, NULL, 0, &r)) return 1;
    uint32_t count = r.mr[1];
    size_t max = sizeof(r.shmem) / sizeof(cc_trace_entry_t);
    if (count > max) count = (uint32_t)max;
    cc_trace_entry_t *e = (cc_trace_entry_t *)r.shmem;
    printf("{\"ok\":%" PRIu32 ",\"events_written\":%" PRIu32
           ",\"bytes_written\":%" PRIu32 ",\"overflow_count\":%" PRIu32
           ",\"events\":[",
           r.mr[0], count, r.mr[2], r.mr[3]);
    for (uint32_t i = 0; i < count; i++) {
        if (i) printf(",");
        printf("{\"timestamp_ns\":%" PRIu64 ",\"from_pd\":%u"
               ",\"to_pd\":%u,\"channel\":%u"
               ",\"opcode\":%u,\"seq_lo\":%u}",
               e[i].timestamp_ns, (unsigned)e[i].from_pd,
               (unsigned)e[i].to_pd, (unsigned)e[i].channel,
               (unsigned)e[i].opcode, (unsigned)e[i].seq_lo);
    }
    printf("]}\n");
    return r.mr[0] == CC_OK ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *env_sock = getenv("CC_PD_SOCK");
    if (env_sock && *env_sock) g_sock_path = env_sock;

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "--socket") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "agentctl: --socket requires PATH\n");
                return 2;
            }
            g_sock_path = argv[i++];
        } else if (strcmp(argv[i], "--batch") == 0) {
            i++;
        } else {
            break;
        }
    }

    if (i >= argc) {
        usage(stderr);
        return 2;
    }

    const char *cmd = argv[i++];
    int n = argc - i;
    char **args = &argv[i];

    if (strcmp(cmd, "inspect") == 0) return n == 0 ? cmd_inspect() : 2;
    if (strcmp(cmd, "frame-capture") == 0)
        return n == 2 ? cmd_frame_capture(parse_u32(args[0], "guest_handle"), args[1]) : 2;
    if (strcmp(cmd,"input-batch")==0) return cmd_input_batch(n,args);
    if (strcmp(cmd, "session-inspect") == 0) return n == 0 ? cmd_session_inspect() : 2;
    if (strcmp(cmd, "connect") == 0) return cmd_connect();
    if (strcmp(cmd, "status") == 0) return cmd_status(n, args);
    if (strcmp(cmd, "list-guests") == 0) return cmd_list_guests();
    if (strcmp(cmd, "guest-status") == 0) return cmd_guest_status(n, args);
    if (strcmp(cmd, "list-devices") == 0) return cmd_list_devices(n, args);
    if (strcmp(cmd, "device-status") == 0 && n >= 2) {
        return cmd_simple(MSG_CC_DEVICE_STATUS,
                          parse_u32(args[0], "dev_type"),
                          parse_u32(args[1], "dev_handle"), 0);
    }
    if (strcmp(cmd, "polecats") == 0 || strcmp(cmd, "list-polecats") == 0) {
        return cmd_simple(MSG_CC_LIST_POLECATS, 0, 0, 0);
    }
    if (strcmp(cmd, "log-stream") == 0 && n >= 2) {
        return cmd_simple(MSG_CC_LOG_STREAM,
                          parse_u32(args[0], "slot"),
                          parse_u32(args[1], "pd_id"), 0);
    }
    if (strcmp(cmd, "fb-attach") == 0 && n >= 2) {
        return cmd_simple(MSG_CC_ATTACH_FRAMEBUFFER,
                          parse_u32(args[0], "guest_handle"),
                          parse_u32(args[1], "fb_handle"), 0);
    }
    if (strcmp(cmd, "send-input") == 0) return cmd_send_input(n, args);
    if (strcmp(cmd, "suspend") == 0 && n >= 1) {
        return cmd_simple(MSG_CC_SUSPEND_GUEST,
                          parse_u32(args[0], "guest_handle"), 0, 0);
    }
    if (strcmp(cmd, "resume") == 0 && n >= 1) {
        return cmd_simple(MSG_CC_RESUME_GUEST,
                          parse_u32(args[0], "guest_handle"), 0, 0);
    }
    if (strcmp(cmd, "destroy") == 0 && n >= 1) {
        return cmd_simple(MSG_CC_DESTROY_GUEST,
                          parse_u32(args[0], "guest_handle"),
                          n > 1 ? parse_u32(args[1], "reason") : GUEST_DESTROY_NORMAL,
                          0);
    }
    if (strcmp(cmd, "snapshot") == 0 && n >= 1) {
        return cmd_simple(MSG_CC_SNAPSHOT, parse_u32(args[0], "guest_handle"),
                          0, 0);
    }
    if (strcmp(cmd, "restore") == 0 && n >= 3) {
        return cmd_simple(MSG_CC_RESTORE,
                          parse_u32(args[0], "guest_handle"),
                          parse_u32(args[1], "snap_lo"),
                          parse_u32(args[2], "snap_hi"));
    }
    if (strcmp(cmd, "trace-start") == 0) {
        return cmd_simple(MSG_CC_TRACE_START,
                          n > 0 ? parse_u32(args[0], "flags") : CC_TRACE_FLAG_WRAP,
                          0, 0);
    }
    if (strcmp(cmd, "trace-stop") == 0) {
        return cmd_simple(MSG_CC_TRACE_STOP, 0, 0, 0);
    }
    if (strcmp(cmd, "trace-query") == 0) {
        return cmd_simple(MSG_CC_TRACE_QUERY, 0, 0, 0);
    }
    if (strcmp(cmd, "trace-dump") == 0) return cmd_trace_dump(n, args);
    if (strcmp(cmd, "raw") == 0 && n >= 1) {
        return cmd_simple(parse_u32(args[0], "opcode"),
                          n > 1 ? parse_u32(args[1], "mr1") : 0,
                          n > 2 ? parse_u32(args[2], "mr2") : 0,
                          n > 3 ? parse_u32(args[3], "mr3") : 0);
    }

    fprintf(stderr, "agentctl: unknown or incomplete command: %s\n", cmd);
    usage(stderr);
    return 2;
}
