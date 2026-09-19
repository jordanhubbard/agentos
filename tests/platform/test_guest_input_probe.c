/* The checker must reject wrong values, order, packet boundaries and extras. */
#define main guest_input_probe_main
#include "../guest/input_probe.c"
#undef main
#include <assert.h>
int main(void)
{
    for (unsigned backpressure=0;backpressure<2;++backpressure)
    for (unsigned device=0;device<2;++device) {
        const expected_event_t *events=device ? (backpressure ? pointer_backpressure : pointer) : keyboard;
        unsigned count=device && !backpressure ? 7 : 4,position=0;
        for (unsigned i=0;i<count;++i) {
            struct input_event e={.type=events[i].type,.code=events[i].code,.value=events[i].value};
            struct input_event bad=e;
            bad.code++;
            assert(!accept_event(backpressure,device,&position,&bad) && position==i);
            bad=e; bad.type++;
            assert(!accept_event(backpressure,device,&position,&bad) && position==i);
            if (e.type!=EV_SYN) {
                bad=e; bad.value++;
                assert(!accept_event(backpressure,device,&position,&bad) && position==i);
            } else e.value=123; /* kernel does not specify SYN values */
            assert(accept_event(backpressure,device,&position,&e) && position==i+1);
        }
        assert(!accept_event(backpressure,device,&position,&(struct input_event){0}));
    }
    puts("PASS: guest evdev checker requires exact key/button/motion values and packet boundaries");
    return 0;
}
