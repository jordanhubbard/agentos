#include <platform/ramfb.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
    uint16_t selected;
    unsigned reads, writes, offset;
    int fail_read, fail_write;
    uint8_t signature[4], features[4], directory[132], config[28];
} fixture_t;

static int select_item(void *arg, uint16_t key)
{
    fixture_t *f = arg;
    f->selected = key; f->offset = 0;
    return 0;
}
static int read_item(void *arg, uint8_t *out, size_t n)
{
    fixture_t *f = arg;
    ++f->reads;
    if (f->fail_read) return -1;
    const uint8_t *p;
    size_t size;
    switch (f->selected) {
    case 0: p = f->signature; size = sizeof f->signature; break;
    case 1: p = f->features; size = sizeof f->features; break;
    case 0x19: p = f->directory; size = sizeof f->directory; break;
    default: return -1;
    }
    if (f->offset > size || n > size - f->offset) return -1;
    memcpy(out, p + f->offset, n); f->offset += n;
    return 0;
}
static int write_item(void *arg, uint16_t key, const uint8_t *p, size_t n)
{
    fixture_t *f = arg;
    assert(key == 0x42 && n == sizeof f->config);
    ++f->writes;
    memcpy(f->config, p, n);
    return f->fail_write ? -1 : 0;
}
static fixture_t fresh(void)
{
    fixture_t f = {.signature = {'Q','E','M','U'}, .features = {3,0,0,0}};
    f.directory[3] = 1; /* directory count */
    f.directory[7] = 28; f.directory[9] = 0x42;
    memcpy(f.directory + 12, "etc/ramfb", 10);
    return f;
}
static int configure(fixture_t *f, uint64_t pa, size_t size,
                     uint32_t w, uint32_t h, uint32_t stride)
{
    aos_ramfb_io_t io = {f, select_item, read_item, write_item};
    return aos_ramfb_configure(&io, pa, size, w, h, stride);
}
static int standard(fixture_t *f)
{
    return configure(f, 0x12345678000ULL, 4u<<20, 1024, 768, 4096);
}
int main(void)
{
    fixture_t f = fresh();
    assert(standard(&f) == AOS_RAMFB_OK && f.writes == 1);
    const uint8_t expected[28] = {
        0,0,1,0x23,0x45,0x67,0x80,0, 0x34,0x32,0x52,0x58,
        0,0,0,0, 0,0,4,0, 0,0,3,0, 0,0,0x10,0
    };
    assert(memcmp(f.config, expected, sizeof expected) == 0);
    f = fresh(); f.signature[0] = 'X';
    assert(standard(&f) == AOS_RAMFB_NO_DEVICE && !f.writes);
    f = fresh(); f.features[0] = 1;
    assert(standard(&f) == AOS_RAMFB_NO_DMA && !f.writes);
    f = fresh(); f.directory[2] = 1; /* 257 exceeds bounded directory */
    assert(standard(&f) == AOS_RAMFB_BAD_DIRECTORY && f.reads == 3);
    f = fresh(); f.directory[7] = 27;
    assert(standard(&f) == AOS_RAMFB_BAD_DIRECTORY && !f.writes);
    f = fresh(); f.directory[9] = 1;
    assert(standard(&f) == AOS_RAMFB_BAD_DIRECTORY && !f.writes);
    f = fresh(); f.directory[3] = 2;
    memcpy(f.directory + 68, f.directory + 4, 64);
    assert(standard(&f) == AOS_RAMFB_BAD_DIRECTORY && !f.writes);
    f = fresh(); f.directory[21] = 'x'; /* prefix match is insufficient */
    assert(standard(&f) == AOS_RAMFB_NO_DEVICE && !f.writes);
    f = fresh(); f.fail_read = 1;
    assert(standard(&f) == AOS_RAMFB_IO_ERROR && !f.writes);
    f = fresh(); f.fail_write = 1;
    assert(standard(&f) == AOS_RAMFB_IO_ERROR && f.writes == 1);
    f = fresh();
    assert(configure(&f, UINT64_MAX-3, 4u<<20, 1024,768,4096) == AOS_RAMFB_BAD_ARGUMENT);
    assert(configure(&f, 0x80000000, 1024, 1024,768,4096) == AOS_RAMFB_BAD_ARGUMENT);
    assert(configure(&f, 0x80000000, 4u<<20, 1024,768,4095) == AOS_RAMFB_BAD_ARGUMENT);
    assert(configure(&f, 0x80000001, 4u<<20, 1024,768,4096) == AOS_RAMFB_BAD_ARGUMENT);
    assert(configure(&f, 0x80000000, 4u<<20, 1025,768,4100) == AOS_RAMFB_BAD_ARGUMENT);
    assert(configure(&f, 0x80000000, 4u<<20, 15,768,4096) == AOS_RAMFB_BAD_ARGUMENT);
    assert(configure(&f, 0x80000000, 4u<<20, 1024,15,4096) == AOS_RAMFB_BAD_ARGUMENT);
    assert(!f.reads && !f.writes);
    puts("ramfb: exact DMA configuration, bounded discovery and failure handling passed");
    return 0;
}
