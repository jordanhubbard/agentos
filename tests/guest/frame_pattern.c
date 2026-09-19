/* SPDX-License-Identifier: BSD-2-Clause
 * Bounded Linux fbdev fixture and matching host reference generator. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <time.h>
#endif

#define WIDTH 1024u
#define HEIGHT 768u
#define BYTES (WIDTH*HEIGHT*4u)

static int pattern(uint8_t *data,const char *mode)
{
    unsigned kind=!strcmp(mode,"tiles") ? 1 : !strcmp(mode,"gradient") ? 2 :
        !strcmp(mode,"noise") ? 3 : 0;
    if (!kind) return -1;
    uint32_t state=0x1729abcd;
    for (unsigned y=0;y<HEIGHT;++y) for (unsigned x=0;x<WIDTH;++x) {
        unsigned r,g,b;
        if (kind==1) {
            r=(x/64u)*29u; g=(y/64u)*47u; b=(x/64u+y/64u)*31u;
        } else if (kind==2) {
            r=x*255u/(WIDTH-1); g=y*255u/(HEIGHT-1);
            b=(x+y)*255u/(WIDTH+HEIGHT-2);
        } else {
            state^=state<<13; state^=state>>17; state^=state<<5;
            r=state>>16; g=state>>8; b=state;
        }
        size_t at=((size_t)y*WIDTH+x)*4u;
        data[at]=(uint8_t)b; data[at+1]=(uint8_t)g;
        data[at+2]=(uint8_t)r; data[at+3]=0;
    }
    return 0;
}

static int transfer(int fd,uint8_t *data,int writing)
{
    size_t done=0;
    while (done<BYTES) {
        ssize_t n=writing ? pwrite(fd,data+done,BYTES-done,(off_t)done) :
            pread(fd,data+done,BYTES-done,(off_t)done);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return -1;
        done+=(size_t)n;
    }
    return 0;
}

#ifdef __linux__
static volatile sig_atomic_t stopping;
static void stop(int signo) { (void)signo; stopping=1; }

static int display(uint8_t *data,const char *mode,unsigned seconds)
{
    int fb=open("/dev/fb0",O_RDWR|O_CLOEXEC), tty=open("/dev/tty1",O_RDWR|O_CLOEXEC|O_NOCTTY);
    int result=1, old_mode=0, graphics=0, saved=0;
    uint8_t *previous=malloc(BYTES);
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    struct vt_stat vt;
    if (fb<0 || tty<0 || !previous || ioctl(fb,FBIOGET_VSCREENINFO,&v)<0 ||
        ioctl(fb,FBIOGET_FSCREENINFO,&f)<0 || ioctl(tty,VT_GETSTATE,&vt)<0 ||
        ioctl(tty,KDGETMODE,&old_mode)<0) { perror("frame fixture setup"); goto out; }
    if (vt.v_active!=1 || old_mode!=KD_TEXT || v.xres!=WIDTH || v.yres!=HEIGHT ||
        v.bits_per_pixel!=32 || v.xoffset || v.yoffset || f.line_length!=WIDTH*4u ||
        f.smem_len<BYTES || v.red.offset!=16 || v.green.offset!=8 || v.blue.offset ||
        v.red.length!=8 || v.green.length!=8 || v.blue.length!=8 ||
        v.red.msb_right || v.green.msb_right || v.blue.msb_right) {
        fprintf(stderr,"requires active text VT1 and 1024x768 XRGB8888 fbdev\n"); goto out;
    }
    struct sigaction action={.sa_handler=stop};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT,&action,NULL)<0 || sigaction(SIGTERM,&action,NULL)<0 ||
        sigaction(SIGHUP,&action,NULL)<0) { perror("signal setup"); goto out; }
    if (ioctl(tty,KDSETMODE,KD_GRAPHICS)<0) { perror("graphics mode"); goto out; }
    graphics=1;
    if (transfer(fb,previous,0)<0) { perror("save framebuffer"); goto out; }
    saved=1;
    if (transfer(fb,data,1)<0) { perror("write framebuffer"); goto out; }
    printf("FRAME_PATTERN_WRITTEN mode=%s bytes=%u pid=%ld hold_seconds=%u\n",
        mode,BYTES,(long)getpid(),seconds);
    fflush(stdout);
    /* A completed write is not a display fence: the host must verify pixels.
     * SIGKILL cannot restore state; ordinary exit and handled signals do. */
    struct timespec begin,now;
    if (clock_gettime(CLOCK_MONOTONIC,&begin)<0) goto out;
    do {
        struct timespec interval={.tv_sec=0,.tv_nsec=100000000};
        nanosleep(&interval,NULL);
        if (clock_gettime(CLOCK_MONOTONIC,&now)<0) goto out;
    } while (!stopping && now.tv_sec-begin.tv_sec<(time_t)seconds);
    result=0;
out:
    if (saved && transfer(fb,previous,1)<0) { perror("restore framebuffer"); result=1; }
    if (graphics && ioctl(tty,KDSETMODE,old_mode)<0) { perror("restore text mode"); result=1; }
    free(previous);
    if (fb>=0) close(fb);
    if (tty>=0) close(tty);
    if (!result) puts("FRAME_PATTERN_RESTORED");
    return result;
}
#endif

int main(int argc,char **argv)
{
    if (argc!=4) {
        fprintf(stderr,"usage: frame-pattern generate MODE NEW_FILE | display MODE SECONDS\n");
        return 2;
    }
    uint8_t *data=malloc(BYTES);
    if (!data) return 1;
    int result=2;
    if (pattern(data,argv[2])<0) { fprintf(stderr,"mode must be tiles, gradient or noise\n"); goto out; }
    if (!strcmp(argv[1],"generate")) {
        int fd=open(argv[3],O_WRONLY|O_CREAT|O_EXCL,0600);
        if (fd<0) { perror("new reference file"); result=1; goto out; }
        result=transfer(fd,data,1)<0;
        if (close(fd)<0) result=1;
        if (result) { perror("reference write"); unlink(argv[3]); }
    } else if (!strcmp(argv[1],"display")) {
#ifdef __linux__
        char *end;
        errno=0;
        unsigned long seconds=strtoul(argv[3],&end,10);
        if (!errno && !*end && seconds>=1 && seconds<=300)
            result=display(data,argv[2],(unsigned)seconds);
        else fprintf(stderr,"hold must be 1..300 seconds\n");
#else
        fprintf(stderr,"display requires Linux fbdev\n");
#endif
    }
out:
    free(data);
    return result;
}
