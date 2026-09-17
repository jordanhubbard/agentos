#include <platform/display_producer.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static aos_display_region_t region;
static uint8_t banks[2][AOS_DISPLAY_FRAME_BYTES],pixels[AOS_DISPLAY_FRAME_BYTES];
static aos_display_driver_t driver;
static unsigned calls,corrupt;
static uint32_t expected_width=1024,expected_height=768,expected_x,expected_y;
static int present(void *unused,unsigned bank,uint32_t w,uint32_t h)
{
    (void)unused; assert(w==expected_width && h==expected_height); ++calls;
    for (uint32_t y=0;y<h;++y)
        assert(memcmp(banks[bank]+y*w*4u,
            pixels+((expected_y+y)*1024u+expected_x)*4u,w*4u)==0);
    return 0;
}
static int exchange(void *unused,const aos_display_request_t *q,aos_display_response_t *p)
{
    (void)unused;
    assert(aos_display_submit(&region,q)==0);
    assert(aos_display_pump(&driver)==1);
    assert(aos_display_receive(&region,p)==0);
    if (corrupt) p->id++;
    return 0;
}
int main(void)
{
    for (size_t i=0;i<sizeof pixels;++i) pixels[i]=(uint8_t)(i*37u+19u);
    assert(aos_display_init(&driver,&region,banks[0],banks[1],sizeof banks[0],present,0)==0);
    /* The producer receives validated private framebuffer state, not a
     * client-supplied pointer. Use a full-size committed surface fixture. */
    aos_fb_client_t client={.selected_handle=9,.selected_width=1024,.selected_height=768};
    client.surfaces[0]=(aos_fb_surface_t){.handle=9,.sequence=1,.width=1024,
        .height=768,.committed=pixels};
    aos_display_producer_t producer={.region=&region,.exchange=exchange};
    assert(aos_display_forward(&producer,&client)==1 && calls==1);
    assert(producer.id==50); /* begin + 48 payloads + present */
    assert(aos_display_forward(&producer,&client)==0 && calls==1 && producer.id==50);
    expected_x=7; expected_y=11; expected_width=1013; expected_height=751;
    client.selected_x=expected_x; client.selected_y=expected_y;
    client.selected_width=expected_width; client.selected_height=expected_height;
    assert(aos_display_forward(&producer,&client)==1 && calls==2);
    assert(producer.driver_sequence==2);
    client.selected_width=1024;
    assert(aos_display_forward(&producer,&client)==-1 && calls==2);
    client.selected_width=expected_width;
    ++client.surfaces[0].sequence; corrupt=1;
    assert(aos_display_forward(&producer,&client)==-1 && producer.failed && calls==2);
    uint32_t requests=producer.id;
    assert(aos_display_forward(&producer,&client)==-1 && producer.id==requests);
    puts("display producer: exact full/cropped frames, chunk boundaries, unchanged frame and reply failure passed");
    return 0;
}
