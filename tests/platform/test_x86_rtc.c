#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform/x86_rtc.h"
static uint32_t io(aos_x86_rtc_t *r,unsigned reg,bool write,uint32_t v,uint64_t t)
{ assert(aos_x86_rtc_io(r,reg,write,&v,t)); return v; }
static void reject(aos_x86_rtc_t *r,unsigned reg,bool write,uint32_t v,uint64_t t)
{
    aos_x86_rtc_t before=*r; uint32_t original=v;
    assert(!aos_x86_rtc_io(r,reg,write,&v,t));
    assert(v==original && !memcmp(&before,r,sizeof(before)));
}
static void init(aos_x86_rtc_t *r)
{ assert(aos_x86_rtc_init(r,AOS_X86_RTC_BOOT_EPOCH,0)); }
int main(void)
{
    aos_x86_rtc_t r,other;
    init(&r); init(&other);
    assert(io(&r,0xa,false,0,0)==0x26 && io(&r,0xb,false,0,0)==2);
    assert(io(&r,0xd,false,0,0)==0x80);
    assert(io(&r,0,false,0,0)==0 && io(&r,6,false,0,0)==7);
    assert(io(&r,7,false,0,0)==1 && io(&r,8,false,0,0)==1);
    assert(io(&r,9,false,0,0)==0 && io(&r,0x32,false,0,0)==0x20);
    uint64_t h=AOS_X86_RTC_HZ;
    assert(io(&r,0xa,false,0,h-875)==0x26);
    assert(io(&r,0xa,false,0,h-874)==0xa6);
    assert(io(&r,0xa,false,0,h)==0x26);
    assert(io(&r,0,false,0,h)==1);
    assert(io(&r,0xc,false,0,h)==0x50); /* periodic + update, no IRQ */
    assert(io(&r,0xc,false,0,h)==0);
    assert(io(&r,0,false,0,61*h)==1 && io(&r,2,false,0,61*h)==1);
    assert(io(&r,7,false,0,86400*h)==2 && io(&r,6,false,0,86400*h)==1);
    assert(io(&other,7,false,0,0)==1); /* independent clock */
    reject(&r,0,false,0,0); /* reversed clock */
    reject(&r,0,true,1,86400*h); /* calendar writes need SET */
    assert(io(&r,1,false,0,86400*h)==0);
    reject(&r,0xa,true,0x66,86400*h); /* divider-stop mode */
    reject(&r,0xb,true,0x42,86400*h); /* PIE */
    reject(&r,0xb,true,0x22,86400*h); /* AIE */
    reject(&r,0xb,true,0x12,86400*h); /* UIE */
    reject(&r,0xb,true,0x0a,86400*h); /* square wave */
    io(&r,0xc,true,0,86400*h); /* read-only register writes are ignored */
    assert(io(&r,0xc,false,0,86400*h)==0x70); /* midnight default alarm */
    io(&r,0xd,true,0,86400*h);
    assert(io(&r,0xd,false,0,86400*h)==0x80);

    init(&r);
    io(&r,0xb,true,0x82,0); /* SET, BCD */
    reject(&r,0,true,0x6a,0);
    reject(&r,4,true,0x24,0);
    io(&r,9,true,0x24,0); io(&r,8,true,2,0); io(&r,7,true,0x28,0);
    io(&r,4,true,0x23,0); io(&r,2,true,0x59,0); io(&r,0,true,0x59,0);
    io(&r,6,true,4,0);
    assert(io(&r,0,false,0,10*h)==0x59); /* frozen during SET */
    assert(io(&r,0xa,false,0,11*h-1)==0x26); /* no UIP during SET */
    assert((io(&r,0xc,false,0,11*h)&0x10)==0);
    io(&r,0xb,true,2,11*h);
    assert(io(&r,7,false,0,12*h)==0x29 && io(&r,6,false,0,12*h)==5);
    assert(io(&r,4,false,0,12*h)==0);
    io(&r,0xb,true,0,12*h); /* 12-hour BCD: midnight is 12 AM */
    assert(io(&r,4,false,0,12*h)==0x12);
    assert(io(&r,4,false,0,(12+12*3600)*h)==0x92); /* noon */

    init(&r);
    io(&r,0xb,true,0x86,0); /* SET, binary, 24 hour */
    io(&r,0x32,true,21,0); io(&r,9,true,0,0);
    io(&r,8,true,2,0); io(&r,7,true,29,0);
    reject(&r,0xb,true,6,0); /* 2100 is not a leap year; transaction retained */
    assert(io(&r,0xb,false,0,0)==0x86);
    io(&r,7,true,28,0); io(&r,4,true,23,0); io(&r,2,true,59,0); io(&r,0,true,59,0);
    io(&r,0xb,true,6,0);
    assert(io(&r,7,false,0,h)==1 && io(&r,8,false,0,h)==3);
    assert(io(&r,0x32,false,0,h)==21);
    io(&r,0xb,true,0x84,h); /* SET, binary, 12 hour */
    io(&r,4,true,0x8c,h); /* noon */
    io(&r,0xb,true,4,h);
    assert(io(&r,4,false,0,h)==0x8c);
    assert(io(&r,4,false,0,h+12*3600*h)==12);
    init(&r);
    io(&r,1,true,0x05,0); io(&r,3,true,0xc0,0); io(&r,5,true,0xff,0);
    assert(io(&r,1,false,0,0)==5 && io(&r,5,false,0,0)==0xff);
    assert((io(&r,0xc,false,0,4*h)&0x20)==0);
    assert((io(&r,0xc,false,0,6*h)&0x20)==0x20);
    assert(io(&r,0xc,false,0,6*h)==0);
    assert((io(&r,0xc,false,0,86400*100*h)&0x20)==0x20);
    reject(&r,1,true,0x6a,86400*100*h);
    init(&r);
    io(&r,0xb,true,0,0); io(&r,5,true,0x92,0); /* BCD 12-hour noon alarm */
    assert((io(&r,0xc,false,0,12*3600*h)&0x20)==0x20);
    init(&r);
    reject(&r,0,false,0,UINT64_MAX);
    assert(!aos_x86_rtc_init(&r,UINT64_MAX,0));
    puts("PASS: RTC elapsed calendar, SET transactions, formats, leap years, flags and rejection");
}
