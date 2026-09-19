/* The checker must reject wrong values, order, packet boundaries and extras. */
#define main guest_input_probe_main
#include "../guest/input_probe.c"
#undef main
#include <assert.h>
int main(void)
{
    for (unsigned first=0;first<2;++first) {
        unsigned positions[2]={0,0};
        bool held=false;
        struct input_event syn={.type=EV_SYN,.code=SYN_REPORT};
        struct input_event down[2]={{.type=EV_KEY,.code=KEY_F12,.value=1},
                                   {.type=EV_KEY,.code=BTN_LEFT,.value=1}};
        for (unsigned order=0;order<2;++order) {
            unsigned device=first^order;
            assert(accept_disconnect_event(device,positions,&held,&down[device]));
            assert(!held);
            assert(accept_disconnect_event(device,positions,&held,&syn));
            assert(held==(order==1));
            if (!held) {
                struct input_event early=down[device]; early.value=0;
                assert(!accept_disconnect_event(device,positions,&held,&early));
                assert(positions[device]==2);
            }
        }
        for (unsigned device=0;device<2;++device) {
            struct input_event repeat=down[device]; repeat.value=2;
            assert(!accept_disconnect_event(device,positions,&held,&repeat));
            struct input_event up=down[device]; up.value=0;
            assert(accept_disconnect_event(device,positions,&held,&up));
            assert(accept_disconnect_event(device,positions,&held,&syn));
            assert(positions[device]==4);
            assert(!accept_disconnect_event(device,positions,&held,&up));
        }
        assert(!accept_disconnect_event(2,positions,&held,&syn));
    }
    unsigned latency_position=0;
    for (unsigned i=0;i<40;++i) {
        struct input_event e={.type=(i&1) ? EV_SYN : EV_KEY,
            .code=(i&1) ? SYN_REPORT : KEY_F12,.value=(i&1) ? 123 : (int)(1-((i/2)&1))};
        struct input_event bad=e;
        bad.code++;
        assert(!accept_latency_event(&latency_position,&bad) && latency_position==i);
        bad=e; bad.type++;
        assert(!accept_latency_event(&latency_position,&bad) && latency_position==i);
        if (!(i&1)) {
            bad=e; bad.value=2;
            assert(!accept_latency_event(&latency_position,&bad) && latency_position==i);
            bad=e; bad.value=1-e.value;
            assert(!accept_latency_event(&latency_position,&bad) && latency_position==i);
        }
        assert(accept_latency_event(&latency_position,&e) && latency_position==i+1);
    }
    assert(!accept_latency_event(&latency_position,&(struct input_event){0}));
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
    for (unsigned device=0;device<2;++device) {
        unsigned count=device ? 5 : 2;
        for (unsigned mask=0;mask<(1u<<(count-1));++mask) {
            unsigned position=0;
            bool pending=false;
            struct input_event syn={.type=EV_SYN,.code=SYN_REPORT,.value=123};
            assert(!accept_gui_event(device,&position,&pending,&syn));
            for (unsigned i=0;i<count;++i) {
                expected_event_t expected=device ? gui_pointer[i] :
                    (expected_event_t){EV_KEY,KEY_F12,i ? 0 : 1};
                struct input_event e={.type=expected.type,.code=expected.code,.value=expected.value};
                struct input_event bad=e;
                bad.value++;
                assert(!accept_gui_event(device,&position,&pending,&bad) && position==i);
                bad=e; bad.code++;
                assert(!accept_gui_event(device,&position,&pending,&bad) && position==i);
                bad=e; bad.type=EV_ABS;
                assert(!accept_gui_event(device,&position,&pending,&bad) && position==i);
                assert(accept_gui_event(device,&position,&pending,&e) && pending);
                if (mask & (1u<<i)) {
                    assert(accept_gui_event(device,&position,&pending,&syn) && !pending);
                    assert(!accept_gui_event(device,&position,&pending,&syn));
                }
            }
            assert(position==count && pending); /* final SYN is mandatory */
            assert(accept_gui_event(device,&position,&pending,&syn) && !pending);
            assert(!accept_gui_event(device,&position,&pending,&syn));
            assert(!accept_gui_event(device,&position,&pending,&(struct input_event){.type=EV_KEY}));
        }
    }
    puts("PASS: guest evdev checker requires exact key/button/motion values and packet boundaries");
    return 0;
}
