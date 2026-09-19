/* SPDX-License-Identifier: BSD-2-Clause
 * Run inside the Linux guest: assert real evdev packets from CC input batches.
 * Names and capabilities are discovered through the standard Linux ABI. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

typedef struct { unsigned short type, code; int value; } expected_event_t;
static const expected_event_t keyboard[] = {
    {EV_KEY,KEY_F13,1},{EV_SYN,SYN_REPORT,0},
    {EV_KEY,KEY_F13,0},{EV_SYN,SYN_REPORT,0}
};
static const expected_event_t pointer[] = {
    {EV_REL,REL_X,17},{EV_REL,REL_Y,-9},{EV_REL,REL_WHEEL,1},
    {EV_KEY,BTN_LEFT,1},{EV_SYN,SYN_REPORT,0},
    {EV_KEY,BTN_LEFT,0},{EV_SYN,SYN_REPORT,0}
};
static const expected_event_t pointer_backpressure[] = {
    {EV_KEY,BTN_LEFT,1},{EV_SYN,SYN_REPORT,0},
    {EV_KEY,BTN_LEFT,0},{EV_SYN,SYN_REPORT,0}
};
static bool accept_event(bool backpressure,bool gui,unsigned device,unsigned *position,const struct input_event *event)
{
    const expected_event_t *expected=device ? (backpressure ? pointer_backpressure : pointer) : keyboard;
    const unsigned count=device && !backpressure ? sizeof(pointer)/sizeof(*pointer) : sizeof(keyboard)/sizeof(*keyboard);
    if (*position>=count) return false;
    const expected_event_t *e=&expected[*position];
    /* Linux defines SYN values as unspecified; packet boundaries and all
     * key/button/motion values are checked, timestamps are not compared. */
    unsigned short code=gui && !device && e->type==EV_KEY ? KEY_F12 : e->code;
    if (event->type!=e->type || event->code!=code ||
        (e->type!=EV_SYN && event->value!=e->value)) return false;
    ++*position;
    return true;
}
static int64_t milliseconds(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)!=0) return -1;
    return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static bool supports(int fd,unsigned type,unsigned code)
{
    unsigned char bits[128]={0};
    return code<sizeof(bits)*8 && ioctl(fd,EVIOCGBIT(type,sizeof(bits)),bits)>=0 &&
        (bits[code/8] & (1u<<(code%8)));
}
int main(int argc,char **argv)
{
    bool backpressure=argc==2 && !strcmp(argv[1],"--backpressure");
    bool gui=argc==2 && !strcmp(argv[1],"--gui");
    if (argc!=1 && !backpressure && !gui) { fprintf(stderr,"usage: input-probe [--backpressure|--gui]\n"); return 2; }
    /* Native GUI qualification uses its supported F12 physical key and a
     * stationary captured click. The CLI/backpressure recipes retain F13. */
    backpressure=backpressure || gui;
    int devices[2]={-1,-1};
    for (unsigned i=0;i<64;++i) {
        char path[64],name[128]={0};
        snprintf(path,sizeof(path),"/dev/input/event%u",i);
        int fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
        if (fd<0) continue;
        if (ioctl(fd,EVIOCGNAME(sizeof(name)-1),name)<0) { close(fd); continue; }
        int device=!strcmp(name,"agentOS keyboard") ? 0 : !strcmp(name,"agentOS pointer") ? 1 : -1;
        if (device<0) { close(fd); continue; }
        if (devices[device]>=0) { fprintf(stderr,"duplicate input device\n"); return 1; }
        devices[device]=fd;
    }
    if (devices[0]<0 || devices[1]<0 || !supports(devices[0],EV_KEY,gui ? KEY_F12 : KEY_F13) ||
        !supports(devices[1],EV_KEY,BTN_LEFT) || !supports(devices[1],EV_REL,REL_X) ||
        !supports(devices[1],EV_REL,REL_Y) || !supports(devices[1],EV_REL,REL_WHEEL)) {
        fprintf(stderr,"missing keyboard/pointer or advertised capabilities\n"); return 1;
    }
    for (unsigned i=0;i<2;++i)
        if (ioctl(devices[i],EVIOCGRAB,1)!=0) { perror("input grab"); return 1; }
    puts("AGENTOS_INPUT_READY"); fflush(stdout);
    int64_t start=milliseconds();
    if (start<0) return 1;
    unsigned positions[2]={0,0};
    while (positions[0]<4 || positions[1]<(backpressure ? 4u : 7u)) {
        int64_t now=milliseconds();
        if (now<0 || now-start>=120000) { fprintf(stderr,"input deadline expired\n"); return 1; }
        struct pollfd fds[2]={{devices[0],POLLIN,0},{devices[1],POLLIN,0}};
        int ready=poll(fds,2,1000);
        if (ready<0) { if (errno==EINTR) continue; perror("input poll"); return 1; }
        for (unsigned i=0;i<2;++i) {
            if (fds[i].revents & (POLLERR|POLLHUP|POLLNVAL)) return 1;
            if (!(fds[i].revents & POLLIN)) continue;
            struct input_event events[32];
            ssize_t bytes=read(devices[i],events,sizeof(events));
            if (bytes<0 && (errno==EINTR || errno==EAGAIN)) continue;
            if (bytes<=0 || bytes%(ssize_t)sizeof(*events)) return 1;
            for (unsigned j=0;j<(size_t)bytes/sizeof(*events);++j)
                if (!accept_event(backpressure,gui,i,&positions[i],&events[j])) {
                    fprintf(stderr,"unexpected device %u event %u: %u/%u/%d\n",i,
                            positions[i],events[j].type,events[j].code,events[j].value);
                    return 1;
                }
        }
    }
    struct pollfd extra[2]={{devices[0],POLLIN,0},{devices[1],POLLIN,0}};
    if (poll(extra,2,200)!=0) { fprintf(stderr,"unexpected trailing input\n"); return 1; }
    close(devices[0]); close(devices[1]);
    puts(gui ? "AGENTOS_GUI_INPUT_PASS keyboard=4 pointer=4" : backpressure ? "AGENTOS_INPUT_PASS keyboard=4 pointer=4" : "AGENTOS_INPUT_PASS keyboard=4 pointer=7");
    return 0;
}
