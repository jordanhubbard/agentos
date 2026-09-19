/* Exercise the real CLI against the observer service over CC wire frames. */
#define main agentctl_main
#include "../../tools/agentctl/agentctl.c"
#undef main
#include <assert.h>
#include <sys/wait.h>

static void serve(int listener, unsigned mode)
{
    int fd = accept(listener, NULL, NULL);
    cc_reply_wire_t hello = {.mr = {CC_CONNECTION_MAGIC, CC_CONNECTION_VERSION, 7, 9}};
    assert(write_full(fd, &hello, sizeof(hello)));
    cc_req_wire_t sync;
    assert(read_full(fd, &sync, sizeof(sync)));
    hello.mr[0] = MSG_CC_CONNECTION_SYNC;
    assert(memcmp(&sync, &hello, sizeof(sync)) == 0);
    hello.mr[0] = CC_OK;
    assert(write_full(fd, &hello, sizeof(hello)));
    assert(fd >= 0);
    aos_fb_observer_region_t region = {0};
    uint8_t committed[40 * 40 * 4];
    uint8_t *storage = malloc(AOS_FB_SURFACE_BYTES);
    assert(storage);
    for (unsigned i = 0; i < sizeof(committed); ++i) committed[i] = (uint8_t)i;
    aos_fb_client_t client = {.selected_handle=1, .selected_width=40, .selected_height=40};
    client.surfaces[0] = (aos_fb_surface_t){.handle=1, .sequence=mode == 3 ? 0 : 17,
        .width=40, .height=40, .committed=committed};
    aos_fb_observer_t observer;
    assert(aos_fb_observer_init(&observer, &region, &client, 1, 1,
                               storage, AOS_FB_SURFACE_BYTES) == 0);
    cc_req_wire_t request;
    while (read_full(fd, &request, sizeof(request))) {
        assert(request.opcode == MSG_CC_FRAME_CAPTURE);
        assert(request.mr[1] == 0 && request.mr[2] == 0);
        aos_fb_observer_request_t query;
        memcpy(&query, request.shmem, sizeof(query));
        assert(query.id == 0 && query.client == 0);
        assert(request.mr[0] == (query.operation == AOS_FB_CAPTURE ? 7u : 0u));
        assert(aos_fb_observer_submit(&region, &query) == 0);
        assert(aos_fb_observer_pump(&observer) == 1);
        aos_fb_observer_response_t response;
        assert(aos_fb_observer_receive(&region, &response) == 0);
        if (query.operation == AOS_FB_CAPTURE) memset(committed, 0, sizeof(committed));
        if (query.operation == AOS_FB_CAPTURE_READ && query.offset && mode == 1)
            ++response.sequence;
        cc_reply_wire_t reply = {.mr={CC_OK, sizeof(response) + response.length,
                                      response.status, AOS_FB_OBSERVER_VERSION}};
        assert(response.length <= sizeof(reply.shmem) - sizeof(response));
        memcpy(reply.shmem + sizeof(response), region.data, response.length);
        if (query.operation == AOS_FB_CAPTURE_READ && mode == 2) response.length = 5000;
        memcpy(reply.shmem, &response, sizeof(response));
        assert(write_full(fd, &reply, sizeof(reply)));
        if (query.operation == AOS_FB_CAPTURE_RELEASE) break;
    }
    free(storage);
    close(fd);
    close(listener);
}

int main(void)
{
    char directory[] = "/tmp/agentctl-frame-test-XXXXXX";
    assert(mkdtemp(directory));
    char socket_path[108], output_path[160];
    assert(snprintf(socket_path, sizeof(socket_path), "%s/socket", directory) > 0);
    assert(snprintf(output_path, sizeof(output_path), "%s/frame.ppm", directory) > 0);
    g_sock_path = socket_path;
    for (unsigned mode = 0; mode < 5; ++mode) {
        int listener = socket(AF_UNIX, SOCK_STREAM, 0);
        assert(listener >= 0);
        struct sockaddr_un address = {.sun_family=AF_UNIX};
        strcpy(address.sun_path, socket_path);
        assert(bind(listener, (void *)&address, sizeof(address)) == 0);
        assert(listen(listener, 1) == 0);
        if (mode == 4) {
            FILE *existing = fopen(output_path, "wx");
            assert(existing && fputs("preserve", existing) >= 0 && fclose(existing) == 0);
        }
        pid_t child = fork();
        assert(child >= 0);
        if (!child) { serve(listener, mode); _exit(0); }
        close(listener);
        assert(cmd_frame_capture(7, output_path) == (mode == 0 ? 0 : 1));
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        assert(unlink(socket_path) == 0);
        if (mode == 0) {
            FILE *file = fopen(output_path, "rb");
            assert(file);
            const char header[] = "P6\n40 40\n255\n";
            for (unsigned i = 0; i < sizeof(header)-1; ++i) assert(fgetc(file) == header[i]);
            for (unsigned i = 0; i < 40*40*4; i += 4) {
                assert(fgetc(file) == (uint8_t)(i+2));
                assert(fgetc(file) == (uint8_t)(i+1));
                assert(fgetc(file) == (uint8_t)i);
            }
            assert(fgetc(file) == EOF && fclose(file) == 0);
            assert(unlink(output_path) == 0);
        } else if (mode == 4) {
            FILE *file = fopen(output_path, "rb");
            char preserved[9] = {0};
            assert(file && fread(preserved, 1, 8, file) == 8 && fclose(file) == 0);
            assert(strcmp(preserved, "preserve") == 0 && unlink(output_path) == 0);
        } else assert(access(output_path, F_OK) != 0);
    }
    assert(rmdir(directory) == 0);
    puts("agentctl frame capture: exact RGB, chunk coherence, malformed replies and output preservation passed");
    return 0;
}
