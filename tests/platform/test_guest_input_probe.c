/* The checker must reject wrong values, order, packet boundaries and extras. */
#define main guest_input_probe_main
#include "../guest/input_probe.c"
#undef main
#include <assert.h>
int main(void)
{
    for (unsigned mode=0;mode<3;++mode)
    for (unsigned device=0;device<2;++device) {
        bool backpressure=mode!=0,gui=mode==2;
        const expected_event_t *events=device ? (backpressure ? pointer_backpressure : pointer) : keyboard;
        unsigned count=device && !backpressure ? 7 : 4,position=0;
        for (unsigned i=0;i<count;++i) {
            struct input_event e={.type=events[i].type,.code=events[i].code,.value=events[i].value};
            if (gui && !device && e.type==EV_KEY) e.code=KEY_F12;
            struct input_event bad=e;
            bad.code++;
            assert(!accept_event(backpressure,gui,device,&position,&bad) && position==i);
            bad=e; bad.type++;
            assert(!accept_event(backpressure,gui,device,&position,&bad) && position==i);
            if (e.type!=EV_SYN) {
                bad=e; bad.value++;
                assert(!accept_event(backpressure,gui,device,&position,&bad) && position==i);
            } else e.value=123; /* kernel does not specify SYN values */
            assert(accept_event(backpressure,gui,device,&position,&e) && position==i+1);
        }
        assert(!accept_event(backpressure,gui,device,&position,&(struct input_event){0}));
    }
    puts("PASS: guest evdev checker requires exact key/button/motion values and packet boundaries");
    return 0;
}
