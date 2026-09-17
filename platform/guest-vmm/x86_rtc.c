#include "platform/x86_rtc.h"
#define LIMIT UINT64_C(253402300800) /* first second of year 10000 */
static bool leap(unsigned y) { return y%4==0 && (y%100!=0 || y%400==0); }
static unsigned month_days(unsigned y, unsigned m)
{
    static const uint8_t days[12]={31,28,31,30,31,30,31,31,30,31,30,31};
    return days[m-1]+(m==2 && leap(y));
}
static void decode(uint64_t seconds, uint8_t f[8])
{
    uint64_t days=seconds/86400;
    f[0]=seconds%60; f[1]=(seconds/60)%60; f[2]=(seconds/3600)%24;
    f[3]=(days+4)%7+1;
    unsigned y=1970, m=1;
    while (days >= 365u+leap(y)) { days-=365u+leap(y); y++; }
    while (days >= month_days(y,m)) { days-=month_days(y,m); m++; }
    f[4]=days+1; f[5]=m; f[6]=y%100; f[7]=y/100;
}
static bool encode(const uint8_t f[8], uint64_t *seconds)
{
    unsigned y=f[7]*100u+f[6], m=f[5];
    if (y<1970 || y>9999 || m<1 || m>12 || f[4]<1 || f[4]>month_days(y,m) ||
        f[0]>59 || f[1]>59 || f[2]>23 || f[3]<1 || f[3]>7) return false;
    uint64_t days=0;
    for (unsigned n=1970; n<y; n++) days+=365u+leap(n);
    for (unsigned n=1; n<m; n++) days+=month_days(y,n);
    *seconds=(days+f[4]-1)*86400+f[2]*3600u+f[1]*60u+f[0];
    return true;
}
bool aos_x86_rtc_init(aos_x86_rtc_t *r, uint64_t epoch, uint64_t ticks)
{
    if (!r || epoch>=LIMIT) return false;
    *r=(aos_x86_rtc_t){.epoch=epoch,.base=ticks,.last=ticks,.a=0x26,.b=2};
    return true;
}
static unsigned rate(uint8_t a)
{
    unsigned rs=a&15;
    return !rs ? 0 : rs==1 ? 256 : rs==2 ? 128 : 1u << (16-rs);
}
static uint64_t periods(uint64_t ticks, unsigned hz)
{ return (ticks/AOS_X86_RTC_HZ)*hz+(ticks%AOS_X86_RTC_HZ)*hz/AOS_X86_RTC_HZ; }
static bool current(aos_x86_rtc_t *r, uint64_t ticks, uint8_t f[8])
{
    if (ticks<r->last) return false;
    unsigned hz=rate(r->a);
    if (hz && periods(ticks,hz)>periods(r->last,hz)) r->flags|=0x40;
    if (r->b&0x80) {
        for (unsigned i=0; i<8; i++) f[i]=r->staged[i];
    } else {
        uint64_t elapsed=(ticks-r->base)/AOS_X86_RTC_HZ;
        if (elapsed>=LIMIT-r->epoch) return false;
        if (elapsed>(r->last-r->base)/AOS_X86_RTC_HZ) r->flags|=0x10;
        decode(r->epoch+elapsed,f);
        f[3]=(f[3]-1+r->weekday_bias)%7+1;
    }
    r->last=ticks;
    return true;
}
static int field(unsigned reg)
{
    switch (reg) {
    case 0: return 0; case 2: return 1; case 4: return 2; case 6: return 3;
    case 7: return 4; case 8: return 5; case 9: return 6; case 0x32: return 7;
    default: return -1;
    }
}
static unsigned output(unsigned v, bool binary)
{ return binary ? v : (v/10)*16+v%10; }
static bool input(unsigned v, bool binary, unsigned *out)
{
    if (!binary && ((v&15)>9 || (v>>4)>9)) return false;
    *out=binary ? v : (v>>4)*10+(v&15);
    return true;
}
bool aos_x86_rtc_io(aos_x86_rtc_t *r, unsigned reg, bool write,
                    uint32_t *value, uint64_t ticks)
{
    if (!r || !value) return false;
    aos_x86_rtc_t next=*r;
    uint8_t f[8];
    if (!current(&next,ticks,f)) return false;
    uint32_t result=*value;
    unsigned v=(uint8_t)*value;
    int n=field(reg);
    if (n>=0) {
        bool binary=(next.b&4)!=0, twelve=n==2 && !(next.b&2);
        if (write) {
            if (!(next.b&0x80)) return false;
            unsigned d;
            if (!input(twelve ? v&0x7f : v,binary,&d)) return false;
            if (twelve) {
                if (!d || d>12) return false;
                d=d%12+((v&0x80) ? 12 : 0);
            }
            static const uint8_t max[8]={59,59,23,7,31,12,99,99};
            if (d>max[n] || ((n==3 || n==4 || n==5) && !d)) return false;
            next.staged[n]=d;
        } else {
            unsigned d=f[n], pm=0;
            if (twelve) { pm=d>=12 ? 0x80 : 0; d=d%12 ? d%12 : 12; }
            result=output(d,binary)|pm;
        }
    } else if (reg==0xa) {
        if (write) {
            if ((v&0x70)!=0x20) return false;
            next.a=v&0x7f; /* UIP is read-only */
        } else {
            bool uip=!(next.b&0x80) && (ticks-next.base)%AOS_X86_RTC_HZ >= AOS_X86_RTC_HZ-874;
            result=next.a|(uip ? 0x80 : 0);
        }
    } else if (reg==0xb) {
        if (write) {
            if (v&~0x86u) return false; /* no interrupt enables, SQWE or DST */
            if (!(next.b&0x80) && (v&0x80))
                for (unsigned i=0; i<8; i++) next.staged[i]=f[i];
            if ((next.b&0x80) && !(v&0x80)) {
                if (!encode(next.staged,&next.epoch)) return false;
                uint8_t canonical[8]; decode(next.epoch,canonical);
                next.weekday_bias=(next.staged[3]+7-canonical[3])%7;
                next.base=ticks;
            }
            next.b=v;
        } else result=next.b;
    } else if (reg==0xc && !write) { result=next.flags; next.flags=0; }
    else if (reg==0xd && !write) result=0x80;
    else return false;
    *r=next; *value=result;
    return true;
}
