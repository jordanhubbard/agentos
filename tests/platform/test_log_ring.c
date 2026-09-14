#include <platform/log_ring.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
static struct { uint64_t before; aos_log_ring_t ring; uint64_t after; } memory;
int main(void)
{
    memory.before = memory.after = UINT64_C(0x1122334455667788);
    aos_log_ring_t *r = &memory.ring;
    r->magic = AOS_LOG_MAGIC;
    assert(aos_log_ring_write(r, "abc") == 3);
    assert(r->head == 3 && !memcmp(r->data, "abc", 3));
    r->head = r->tail = AOS_LOG_DATA - 2;
    assert(aos_log_ring_write(r, "wrap") == 4);
    assert(r->head == 2 && r->data[AOS_LOG_DATA - 2] == 'w' &&
           r->data[AOS_LOG_DATA - 1] == 'r' && r->data[0] == 'a' && r->data[1] == 'p');
    r->tail = 3;
    assert(aos_log_ring_write(r, "full") == 0 && r->head == 2);
    r->tail = 5;
    assert(aos_log_ring_write(r, "part") == 2 && r->head == 4);
    assert(r->data[2] == 'p' && r->data[3] == 'a');
    aos_log_ring_t saved = *r;
    r->head = UINT32_MAX;
    assert(aos_log_ring_write(r, "invalid") == 0 && r->head == UINT32_MAX);
    assert(!memcmp(r->data, saved.data, AOS_LOG_DATA));
    r->head = 4; r->tail = AOS_LOG_DATA;
    assert(aos_log_ring_write(r, "invalid") == 0 && r->tail == AOS_LOG_DATA);
    r->head = r->tail = 0;
    char long_message[AOS_LOG_DATA + 10]; memset(long_message, 'z', sizeof(long_message));
    assert(aos_log_ring_write(r, long_message) == AOS_LOG_DATA - 1);
    assert(r->head == AOS_LOG_DATA - 1 && r->tail == 0);
    assert(memory.before == UINT64_C(0x1122334455667788) && memory.after == memory.before);
    puts("PASS: log producer exact bytes, wrap, full/partial drop, invalid cursors, bounded scan");
}
