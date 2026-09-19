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
/* One transition per packet makes each flushed receipt unambiguous. */
static bool accept_latency_event(unsigned *position,const struct input_event *event)
{
    if (*position>=40) return false;
    bool syn=(*position & 1)!=0;
    if (syn ? (event->type!=EV_SYN || event->code!=SYN_REPORT) :
        (event->type!=EV_KEY || event->code!=KEY_F12 ||
         event->value!=(int)(1-((*position/2)&1)))) return false;
    ++*position;
    return true;
}
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
static const expected_event_t gui_pointer[] = {
    {EV_REL,REL_X,17},{EV_REL,REL_Y,-9},{EV_REL,REL_WHEEL,1},
    {EV_KEY,BTN_LEFT,1},{EV_KEY,BTN_LEFT,0}
};
/* GUI batches may group adjacent transitions, but must terminate each packet
 * with SYN_REPORT. Values and ordering remain exact; empty packets fail. */
static bool accept_gui_event(unsigned device,unsigned *position,bool *pending,
                             const struct input_event *event)
{
    unsigned count=device ? 5 : 2;
    if (event->type==EV_SYN) {
        if (event->code!=SYN_REPORT || !*pending) return false;
        *pending=false;
        return true;
    }
    if (*position>=count) return false;
    expected_event_t expected=device ? gui_pointer[*position] :
        (expected_event_t){EV_KEY,KEY_F12,*position ? 0 : 1};
    if (event->type!=expected.type || event->code!=expected.code ||
        event->value!=expected.value) return false;
    ++*position;
    *pending=true;
    return true;
}
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
/* Both complete down packets must be observed before either release. The
 * host waits for this milestone before terminating the GUI process. */
static bool accept_disconnect_event(unsigned device,unsigned positions[2],bool *held,
                                    bool *repeat_pending,unsigned *repeats,
                                    const struct input_event *event)
{
    if (device>1) return false;
    /* Linux can repeat a held key independently of the host. Require each
     * repeat's packet boundary without advancing the release sequence. */
    if (device==0 && *repeat_pending) {
        if (event->type!=EV_SYN || event->code!=SYN_REPORT) return false;
        *repeat_pending=false;
        ++*repeats;
        return true;
    }
    if (device==0 && positions[0]==2 && event->type==EV_KEY &&
        event->code==KEY_F12 && event->value==2) {
        *repeat_pending=true;
        return true;
    }
    if (!*held && positions[device]>=2) return false;
    if (!accept_event(true,true,device,&positions[device],event)) return false;
    if (positions[0]==2 && positions[1]==2) *held=true;
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
    /* Measurement-channel control: no evdev access or injected input. */
    if (argc==2 && !strcmp(argv[1],"--ssh-control")) {
        puts("AGENTOS_INPUT_READY"); fflush(stdout);
        for (unsigned sequence=1;sequence<=20;++sequence) {
            char line[64],expected[64];
            snprintf(expected,sizeof(expected),"AGENTOS_INPUT_PING %u\n",sequence);
            if (!fgets(line,sizeof(line),stdin) || strcmp(line,expected)) return 1;
            printf("AGENTOS_INPUT_ACK %u\n",sequence); fflush(stdout);
        }
        puts("AGENTOS_SSH_CONTROL_PASS receipts=20");
        return 0;
    }
    bool backpressure=argc==2 && !strcmp(argv[1],"--backpressure");
    bool gui=argc==2 && !strcmp(argv[1],"--gui");
    bool motion=argc==2 && !strcmp(argv[1],"--gui-pointer");
    bool latency=argc==2 && !strcmp(argv[1],"--gui-latency");
    bool disconnect=argc==2 && !strcmp(argv[1],"--gui-disconnect");
    if (argc!=1 && !backpressure && !gui && !motion && !latency && !disconnect) { fprintf(stderr,"usage: input-probe [--backpressure|--gui|--gui-pointer|--gui-latency|--gui-disconnect|--ssh-control]\n"); return 2; }
    gui=gui || motion || latency || disconnect;
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
    bool pending[2]={false,false};
    bool held=false,held_reported=false;
    bool repeat_pending=false;
    unsigned repeats=0;
    while (latency ? positions[0]<40 :
           (positions[0]<(motion ? 2u : 4u) || positions[1]<(motion ? 5u : backpressure ? 4u : 7u) || pending[0] || pending[1])) {
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
            for (unsigned j=0;j<(size_t)bytes/sizeof(*events);++j) {
                if (!(disconnect ? accept_disconnect_event(i,positions,&held,&repeat_pending,&repeats,&events[j]) : latency ? (i==0 && accept_latency_event(&positions[0],&events[j])) : motion ? accept_gui_event(i,&positions[i],&pending[i],&events[j]) :
                      accept_event(backpressure,gui,i,&positions[i],&events[j]))) {
                    fprintf(stderr,"unexpected device %u event %u: %u/%u/%d\n",i,
                            positions[i],events[j].type,events[j].code,events[j].value);
                    return 1;
                }
                if (disconnect && held && !held_reported) {
                    puts("AGENTOS_GUI_HELD keyboard=F12 pointer=left");
                    fflush(stdout);
                    held_reported=true;
                }
                if (latency && !(positions[0]&1)) {
                    printf("AGENTOS_INPUT_ACK %u\n",positions[0]/2);
                    fflush(stdout);
                }
            }
        }
    }
    struct pollfd extra[2]={{devices[0],POLLIN,0},{devices[1],POLLIN,0}};
    if (poll(extra,2,200)!=0) { fprintf(stderr,"unexpected trailing input\n"); return 1; }
    close(devices[0]); close(devices[1]);
    if (disconnect) printf("AGENTOS_GUI_REPEAT_PACKETS %u\n",repeats);
    puts(disconnect ? "AGENTOS_GUI_DISCONNECT_PASS keyboard=4 pointer=4 held_before_release=true" : latency ? "AGENTOS_GUI_LATENCY_PASS transitions=20" : motion ? "AGENTOS_GUI_POINTER_PASS key=F12 x=17 y=-9 wheel=1 button=left packets=complete" : gui ? "AGENTOS_GUI_INPUT_PASS keyboard=4 pointer=4" : backpressure ? "AGENTOS_INPUT_PASS keyboard=4 pointer=4" : "AGENTOS_INPUT_PASS keyboard=4 pointer=7");
    return 0;
}
