/* Real log encoder -> real serial dispatcher -> captured UART bytes. */
#include <assert.h>
#include <stdio.h>
#include "log_serial_driver.h"

#define LOG_DRAIN_TEST_CALL log_serial_call
#include "../../services/log-drain/log_drain.c"

static uint8_t shared[4096];
static uint8_t rings[MAX_LOG_RINGS * RING_SIZE];
static char output[4096];
static size_t output_len;
static unsigned write_calls;

void log_serial_capture(char c)
{
    assert(output_len < sizeof(output));
    output[output_len++] = c;
}

void log_serial_call(seL4_CPtr ep, const sel4_msg_t *req, sel4_msg_t *rep)
{
    assert(ep == 1);
    if (req->opcode == MSG_SERIAL_OPEN) {
        assert(req->length == 4);
        assert(data_rd32(req->data, 0) == 0);
    } else {
        assert(req->opcode == MSG_SERIAL_WRITE);
        assert(req->length == 8);
        assert(data_rd32(req->data, 4) <= SERIAL_MAX_WRITE_BYTES);
        write_calls++;
    }
    rep->opcode = log_serial_driver_call(req->opcode, req->length, req->data,
                                        &rep->length, rep->data);
}

static void setup(void)
{
    memset(shared, 0xA5, sizeof(shared));
    memset(rings, 0, sizeof(rings));
    output_len = 0;
    write_calls = 0;
    log_serial_driver_init(shared);
    log_drain_rings_vaddr = (uintptr_t)rings;
    serial_shmem_vaddr = (uintptr_t)shared;
    log_drain_test_init();
    g_serial_ep = 1;
}

static void reserve_slot(void)
{
    uint8_t data[48] = {0}, reply[48] = {0};
    uint32_t length = 0;
    assert(log_serial_driver_call(MSG_SERIAL_OPEN, 4, data, &length, reply)
           == SEL4_ERR_OK);
}

int main(void)
{
    setup();
    reserve_slot(); /* Exercise a nonzero slot and preserve another client's data. */
    uart_puts("release log\n");
    assert(serial_ready && serial_slot == 1);
    assert(output_len == strlen("release log\n"));
    assert(memcmp(output, "release log\n", output_len) == 0);
    assert(write_calls == 1);
    for (size_t i = 0; i < SERIAL_SHMEM_SLOT_STRIDE; i++) assert(shared[i] == 0xA5);

    output_len = 0;
    write_calls = 0;
    char long_line[601];
    for (size_t i = 0; i < sizeof(long_line) - 1; i++) long_line[i] = 'a' + i % 26;
    long_line[600] = 0;
    uart_puts(long_line);
    assert(output_len == 600 && write_calls == 3);
    assert(memcmp(output, long_line, 600) == 0);

    /* Exercise ring drain, including its tag, through the same UART sink. */
    setup();
    ld_ring_header_t *hdr = (ld_ring_header_t *)rings;
    hdr->magic = LOG_RING_MAGIC;
    memcpy(rings + RING_HEADER_SIZE, "hello\n", 6);
    hdr->head = 6;
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_LOG_WRITE;
    req.length = 8;
    data_wr32(req.data, 4, 13); /* log_drain */
    assert(log_drain_dispatch_one(0, &req, &rep) == SEL4_ERR_OK);
    const char expected[] = "\033[36m[log_drain]\033[0m hello\n";
    assert(output_len == sizeof(expected) - 1);
    assert(memcmp(output, expected, output_len) == 0);
    assert(hdr->tail == hdr->head);

    setup();
    for (unsigned i = 0; i < SERIAL_MAX_CLIENTS; i++) reserve_slot();
    uart_puts("no free slot\n");
    assert(!serial_ready && output_len == 0 && write_calls == 0);
    puts("PASS: log_drain to serial_pd UART round-trip, slot isolation, chunking, ring drain, exhaustion");
    return 0;
}
