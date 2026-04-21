/*
 * agentOS serial_pd — Contract Unit Test
 *
 * Tests all IPC opcodes (success + error paths) without seL4 or Microkit.
 * UART hardware I/O is stubbed; the test exercises client-slot lifecycle,
 * write/read multiplexing, status reporting, and configuration validation.
 *
 * Build:  cc -o /tmp/test_serial_pd \
 *             tests/test_serial_pd.c \
 *             -I kernel/agentos-root-task/include \
 *             -DAGENTOS_TEST_HOST
 * Run:    /tmp/test_serial_pd
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Host-side Microkit stubs — must come before any agentos.h include
 * ══════════════════════════════════════════════════════════════════════════ */

static uint64_t _mrs[64];
static inline void     microkit_mr_set(uint32_t i, uint64_t v) { _mrs[i] = v; }
static inline uint64_t microkit_mr_get(uint32_t i)             { return _mrs[i]; }
typedef uint32_t microkit_channel;
typedef uint64_t microkit_msginfo;
static inline microkit_msginfo microkit_msginfo_new(uint64_t l, uint32_t c) {
    (void)c; return l;
}
static inline void microkit_dbg_puts(const char *s) { (void)s; }

/* ══════════════════════════════════════════════════════════════════════════
 * Serial contract constants — inlined to avoid microkit.h pull-in
 * These must stay in sync with agentos.h and serial_contract.h.
 * ══════════════════════════════════════════════════════════════════════════ */

/* Opcodes (from agentos.h) */
#define MSG_SERIAL_OPEN       0x2001u
#define MSG_SERIAL_CLOSE      0x2002u
#define MSG_SERIAL_WRITE      0x2003u
#define MSG_SERIAL_READ       0x2004u
#define MSG_SERIAL_STATUS     0x2005u
#define MSG_SERIAL_CONFIGURE  0x2006u

/* Configuration (from serial_contract.h) */
#define SERIAL_MAX_WRITE_BYTES  256u
#define SERIAL_MAX_CLIENTS      8u

/* Error codes */
#define SERIAL_OK               0u
#define SERIAL_ERR_NO_SLOTS     1u
#define SERIAL_ERR_BAD_PORT     2u
#define SERIAL_ERR_BAD_SLOT     3u
#define SERIAL_ERR_BAD_BAUD     4u
#define SERIAL_ERR_NOT_OPEN     5u

/* Configure flags */
#define SERIAL_FLAG_PARITY_NONE  0x00u
#define SERIAL_FLAG_PARITY_EVEN  0x01u
#define SERIAL_FLAG_PARITY_ODD   0x02u
#define SERIAL_FLAG_STOP_1       0x00u
#define SERIAL_FLAG_STOP_2       0x10u

/* ══════════════════════════════════════════════════════════════════════════
 * Inline data types from serial_pd.c (enough to test IPC logic)
 * ══════════════════════════════════════════════════════════════════════════ */

/* Stub UART: no hardware, writes go to a local TX buffer */
static char tx_buf[4096];
static uint32_t tx_pos = 0;

#define UART_STUB_PUTC(c) do { \
    if (tx_pos < sizeof(tx_buf) - 1) tx_buf[tx_pos++] = (c); \
} while (0)

/* ─── Inline serial_pd state ─────────────────────────────────────────── */

#define RX_BUF_SIZE  512u

typedef struct {
    uint8_t  data[RX_BUF_SIZE];
    uint32_t head;
    uint32_t tail;
} rx_ring_t;

typedef struct {
    bool      open;
    uint32_t  port_id;
    uint32_t  baud;
    uint32_t  flags;
    uint32_t  rx_count;
    uint32_t  tx_count;
    uint32_t  err_flags;
    rx_ring_t rx;
} serial_client_t;

static serial_client_t clients[SERIAL_MAX_CLIENTS];
static bool hw_ready = false;

/* Shared data buffers (simulate shmem) */
static uint8_t shmem[SERIAL_MAX_WRITE_BYTES];

/* ─── Inline IPC handlers (mirrors serial_pd.c logic) ───────────────── */

static void handle_open(void)
{
    uint32_t port_id = (uint32_t)microkit_mr_get(1);
    for (uint32_t i = 0; i < SERIAL_MAX_CLIENTS; i++) {
        if (clients[i].open) continue;
        clients[i].open      = true;
        clients[i].port_id   = port_id;
        clients[i].baud      = 115200;
        clients[i].flags     = 0;
        clients[i].rx_count  = 0;
        clients[i].tx_count  = 0;
        clients[i].err_flags = 0;
        clients[i].rx.head   = 0;
        clients[i].rx.tail   = 0;
        microkit_mr_set(0, SERIAL_OK);
        microkit_mr_set(1, i);
        return;
    }
    microkit_mr_set(0, SERIAL_ERR_NO_SLOTS);
    microkit_mr_set(1, 0);
}

static void handle_close(void)
{
    uint32_t slot = (uint32_t)microkit_mr_get(1);
    if (slot >= SERIAL_MAX_CLIENTS || !clients[slot].open) {
        microkit_mr_set(0, SERIAL_ERR_BAD_SLOT);
        return;
    }
    clients[slot].open = false;
    microkit_mr_set(0, SERIAL_OK);
}

static void handle_write(void)
{
    uint32_t slot = (uint32_t)microkit_mr_get(1);
    uint32_t len  = (uint32_t)microkit_mr_get(2);
    if (slot >= SERIAL_MAX_CLIENTS || !clients[slot].open) {
        microkit_mr_set(0, SERIAL_ERR_BAD_SLOT);
        microkit_mr_set(1, 0);
        return;
    }
    if (len > SERIAL_MAX_WRITE_BYTES) len = SERIAL_MAX_WRITE_BYTES;
    for (uint32_t i = 0; i < len; i++)
        UART_STUB_PUTC((char)shmem[i]);
    clients[slot].tx_count += len;
    microkit_mr_set(0, SERIAL_OK);
    microkit_mr_set(1, len);
}

static void rx_push(rx_ring_t *r, uint8_t c)
{
    uint32_t next = (r->head + 1) % RX_BUF_SIZE;
    if (next != r->tail) { r->data[r->head] = c; r->head = next; }
}

static uint32_t rx_drain(rx_ring_t *r, uint8_t *dst, uint32_t max)
{
    uint32_t n = 0;
    while (n < max && r->tail != r->head) {
        dst[n++] = r->data[r->tail];
        r->tail  = (r->tail + 1) % RX_BUF_SIZE;
    }
    return n;
}

static void handle_read(void)
{
    uint32_t slot = (uint32_t)microkit_mr_get(1);
    uint32_t max  = (uint32_t)microkit_mr_get(2);
    if (slot >= SERIAL_MAX_CLIENTS || !clients[slot].open) {
        microkit_mr_set(0, SERIAL_ERR_BAD_SLOT);
        microkit_mr_set(1, 0);
        return;
    }
    if (max > RX_BUF_SIZE) max = RX_BUF_SIZE;
    uint32_t n = rx_drain(&clients[slot].rx, shmem, max);
    microkit_mr_set(0, SERIAL_OK);
    microkit_mr_set(1, n);
}

static void handle_status(void)
{
    uint32_t slot = (uint32_t)microkit_mr_get(1);
    if (slot >= SERIAL_MAX_CLIENTS || !clients[slot].open) {
        microkit_mr_set(0, SERIAL_ERR_BAD_SLOT);
        return;
    }
    microkit_mr_set(0, SERIAL_OK);
    microkit_mr_set(1, clients[slot].baud);
    microkit_mr_set(2, clients[slot].rx_count);
    microkit_mr_set(3, clients[slot].tx_count);
    microkit_mr_set(4, clients[slot].err_flags);
}

static void handle_configure(void)
{
    uint32_t slot  = (uint32_t)microkit_mr_get(1);
    uint32_t baud  = (uint32_t)microkit_mr_get(2);
    uint32_t flags = (uint32_t)microkit_mr_get(3);
    if (slot >= SERIAL_MAX_CLIENTS || !clients[slot].open) {
        microkit_mr_set(0, SERIAL_ERR_BAD_SLOT);
        return;
    }
    if (baud != 9600 && baud != 38400 && baud != 57600 && baud != 115200) {
        microkit_mr_set(0, SERIAL_ERR_BAD_BAUD);
        return;
    }
    clients[slot].baud  = baud;
    clients[slot].flags = flags;
    microkit_mr_set(0, SERIAL_OK);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test helpers
 * ══════════════════════════════════════════════════════════════════════════ */

#define PASS(name) printf("[PASS] %s\n", name)
#define FAIL(name) do { printf("[FAIL] %s\n", name); return 1; } while (0)

static void reset_state(void)
{
    for (uint32_t i = 0; i < SERIAL_MAX_CLIENTS; i++)
        clients[i].open = false;
    tx_pos = 0;
}

/* Open a slot and return its index; asserts on failure */
static uint32_t do_open(uint32_t port_id)
{
    microkit_mr_set(0, MSG_SERIAL_OPEN);
    microkit_mr_set(1, port_id);
    handle_open();
    assert(microkit_mr_get(0) == SERIAL_OK);
    return (uint32_t)microkit_mr_get(1);
}

static void do_close(uint32_t slot)
{
    microkit_mr_set(0, MSG_SERIAL_CLOSE);
    microkit_mr_set(1, slot);
    handle_close();
    assert(microkit_mr_get(0) == SERIAL_OK);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_open_close(void)
{
    reset_state();

    uint32_t s0 = do_open(0);
    assert(s0 < SERIAL_MAX_CLIENTS);
    assert(clients[s0].open);
    assert(clients[s0].port_id == 0);
    assert(clients[s0].baud == 115200);

    uint32_t s1 = do_open(1);
    assert(s1 != s0);
    assert(clients[s1].port_id == 1);

    do_close(s0);
    assert(!clients[s0].open);

    /* Reopen should reuse the freed slot */
    uint32_t s2 = do_open(2);
    assert(s2 == s0);

    PASS("test_open_close");
    return 0;
}

static int test_open_exhaustion(void)
{
    reset_state();

    uint32_t slots[SERIAL_MAX_CLIENTS];
    for (uint32_t i = 0; i < SERIAL_MAX_CLIENTS; i++)
        slots[i] = do_open(i);

    /* One more should fail */
    microkit_mr_set(0, MSG_SERIAL_OPEN);
    microkit_mr_set(1, 99);
    handle_open();
    if (microkit_mr_get(0) != SERIAL_ERR_NO_SLOTS) FAIL("test_open_exhaustion");

    for (uint32_t i = 0; i < SERIAL_MAX_CLIENTS; i++)
        do_close(slots[i]);

    PASS("test_open_exhaustion");
    return 0;
}

static int test_write_basic(void)
{
    reset_state();
    uint32_t slot = do_open(0);

    const char msg[] = "hello serial_pd";
    uint32_t len = (uint32_t)(sizeof(msg) - 1);
    memcpy(shmem, msg, len);

    microkit_mr_set(0, MSG_SERIAL_WRITE);
    microkit_mr_set(1, slot);
    microkit_mr_set(2, len);
    handle_write();

    if (microkit_mr_get(0) != SERIAL_OK) FAIL("test_write_basic: ok");
    if (microkit_mr_get(1) != len)       FAIL("test_write_basic: written");
    if (clients[slot].tx_count != len)   FAIL("test_write_basic: tx_count");

    /* TX stub captured the bytes */
    if (tx_pos != len)                   FAIL("test_write_basic: tx_pos");
    if (memcmp(tx_buf, msg, len) != 0)   FAIL("test_write_basic: tx_data");

    do_close(slot);
    PASS("test_write_basic");
    return 0;
}

static int test_write_clamp(void)
{
    reset_state();
    uint32_t slot = do_open(0);

    microkit_mr_set(0, MSG_SERIAL_WRITE);
    microkit_mr_set(1, slot);
    microkit_mr_set(2, SERIAL_MAX_WRITE_BYTES + 100);  /* too large */
    handle_write();

    if (microkit_mr_get(0) != SERIAL_OK)               FAIL("test_write_clamp: ok");
    if (microkit_mr_get(1) != SERIAL_MAX_WRITE_BYTES)  FAIL("test_write_clamp: clamped");

    do_close(slot);
    PASS("test_write_clamp");
    return 0;
}

static int test_write_bad_slot(void)
{
    reset_state();

    microkit_mr_set(0, MSG_SERIAL_WRITE);
    microkit_mr_set(1, SERIAL_MAX_CLIENTS);  /* out of range */
    microkit_mr_set(2, 4);
    handle_write();
    if (microkit_mr_get(0) != SERIAL_ERR_BAD_SLOT) FAIL("test_write_bad_slot: oob");

    /* Closed slot */
    microkit_mr_set(1, 0);
    handle_write();
    if (microkit_mr_get(0) != SERIAL_ERR_BAD_SLOT) FAIL("test_write_bad_slot: closed");

    PASS("test_write_bad_slot");
    return 0;
}

static int test_read_rx_ring(void)
{
    reset_state();
    uint32_t slot = do_open(0);

    /* Inject bytes directly into the RX ring */
    const uint8_t payload[] = { 'A', 'B', 'C', 'D' };
    for (uint32_t i = 0; i < sizeof(payload); i++) {
        rx_push(&clients[slot].rx, payload[i]);
        clients[slot].rx_count++;
    }

    microkit_mr_set(0, MSG_SERIAL_READ);
    microkit_mr_set(1, slot);
    microkit_mr_set(2, sizeof(payload));
    handle_read();

    if (microkit_mr_get(0) != SERIAL_OK)           FAIL("test_read_rx_ring: ok");
    if (microkit_mr_get(1) != sizeof(payload))     FAIL("test_read_rx_ring: count");
    if (memcmp(shmem, payload, sizeof(payload)) != 0) FAIL("test_read_rx_ring: data");

    /* Ring should be empty now */
    microkit_mr_set(2, 4);
    handle_read();
    if (microkit_mr_get(1) != 0) FAIL("test_read_rx_ring: empty");

    do_close(slot);
    PASS("test_read_rx_ring");
    return 0;
}

static int test_status(void)
{
    reset_state();
    uint32_t slot = do_open(0);

    /* Write 10 bytes to bump tx_count */
    memset(shmem, 'x', 10);
    microkit_mr_set(0, MSG_SERIAL_WRITE);
    microkit_mr_set(1, slot);
    microkit_mr_set(2, 10);
    handle_write();

    microkit_mr_set(0, MSG_SERIAL_STATUS);
    microkit_mr_set(1, slot);
    handle_status();

    if (microkit_mr_get(0) != SERIAL_OK)  FAIL("test_status: ok");
    if (microkit_mr_get(1) != 115200)     FAIL("test_status: baud");
    if (microkit_mr_get(3) != 10)         FAIL("test_status: tx_count");

    do_close(slot);
    PASS("test_status");
    return 0;
}

static int test_configure(void)
{
    reset_state();
    uint32_t slot = do_open(0);

    microkit_mr_set(0, MSG_SERIAL_CONFIGURE);
    microkit_mr_set(1, slot);
    microkit_mr_set(2, 9600);
    microkit_mr_set(3, SERIAL_FLAG_PARITY_EVEN | SERIAL_FLAG_STOP_2);
    handle_configure();

    if (microkit_mr_get(0) != SERIAL_OK)          FAIL("test_configure: ok");
    if (clients[slot].baud != 9600)               FAIL("test_configure: baud");
    if (clients[slot].flags != (SERIAL_FLAG_PARITY_EVEN | SERIAL_FLAG_STOP_2))
        FAIL("test_configure: flags");

    /* Bad baud */
    microkit_mr_set(2, 12345);
    handle_configure();
    if (microkit_mr_get(0) != SERIAL_ERR_BAD_BAUD) FAIL("test_configure: bad_baud");

    do_close(slot);
    PASS("test_configure");
    return 0;
}

static int test_close_bad_slot(void)
{
    reset_state();

    microkit_mr_set(0, MSG_SERIAL_CLOSE);
    microkit_mr_set(1, 0);  /* slot 0 is not open */
    handle_close();
    if (microkit_mr_get(0) != SERIAL_ERR_BAD_SLOT) FAIL("test_close_bad_slot: closed");

    microkit_mr_set(1, SERIAL_MAX_CLIENTS + 5);  /* out of range */
    handle_close();
    if (microkit_mr_get(0) != SERIAL_ERR_BAD_SLOT) FAIL("test_close_bad_slot: oob");

    PASS("test_close_bad_slot");
    return 0;
}

static int test_multi_port_isolation(void)
{
    reset_state();

    uint32_t s0 = do_open(0);  /* port_id 0 */
    uint32_t s1 = do_open(1);  /* port_id 1 */

    /* Write to s0 */
    const char msg0[] = "port0";
    memcpy(shmem, msg0, sizeof(msg0) - 1);
    microkit_mr_set(0, MSG_SERIAL_WRITE);
    microkit_mr_set(1, s0);
    microkit_mr_set(2, sizeof(msg0) - 1);
    handle_write();
    if (clients[s0].tx_count != sizeof(msg0) - 1) FAIL("test_multi_port: s0.tx");
    if (clients[s1].tx_count != 0)                FAIL("test_multi_port: s1 unchanged");

    do_close(s0);
    do_close(s1);
    PASS("test_multi_port_isolation");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("[serial_pd] contract unit tests\n");

    int failures = 0;
    failures += test_open_close();
    failures += test_open_exhaustion();
    failures += test_write_basic();
    failures += test_write_clamp();
    failures += test_write_bad_slot();
    failures += test_read_rx_ring();
    failures += test_status();
    failures += test_configure();
    failures += test_close_bad_slot();
    failures += test_multi_port_isolation();

    if (failures == 0)
        printf("\n[serial_pd] ALL TESTS PASSED\n");
    else
        printf("\n[serial_pd] %d TEST(S) FAILED\n", failures);

    return failures ? 1 : 0;
}
