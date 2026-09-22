#include <platform/serial_uart.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t output[16], input[]={0xa1,0xa2}, status;
static unsigned out_n, in_n, writes, init_step;
static bool initializing, io_failed;
static bool read_port(void *ctx, unsigned offset, uint8_t *value)
{
    (void)ctx;
    if (io_failed) return false;
    assert(offset==0 || offset==5);
    if (offset==5) *value=status;
    else { assert(in_n<sizeof(input)); *value=input[in_n++]; }
    return true;
}
static bool write_port(void *ctx, unsigned offset, uint8_t value)
{
    (void)ctx;
    if (io_failed) return false;
    writes++;
    if (initializing) {
        const uint8_t offsets[]={1,3,0,1,3,2,4};
        const uint8_t values[]={0,0x80,3,0,3,0xc7,0x0b};
        assert(init_step<7 && offset==offsets[init_step] && value==values[init_step]);
        init_step++;
    } else {
        assert(offset==0 && out_n<sizeof(output)); output[out_n++]=value;
    }
    return true;
}
int main(void)
{
    const aos_serial_uart_io_t io={.read=read_port,.write=write_port};
    initializing=true;
    assert(aos_serial_uart_init(&io) && init_step==7 && writes==7);
    initializing=false;
    aos_serial_queue_t txq={0},rxq={0};
    uint8_t tx[4]={0},rx[4]={0};
    aos_serial_channel_meta_t meta={0};
    aos_serial_uart_t uart={.channel={.from_guest={&txq,tx,4},.to_guest={&rxq,rx,4},.meta=&meta}};
    const uint8_t bytes[]={1,2,3,4};
    assert(aos_serial_queue_write(&uart.channel.from_guest,bytes,4)==AOS_SERIAL_PUMP_OK);
    bool changed=false;
    assert(aos_serial_uart_step(&uart,&io,&changed) && changed && uart.tx_pending && !out_n);
    assert(aos_serial_uart_step(&uart,&io,&changed) && !changed && txq.head==1 && !out_n);
    status=0x20;
    for (unsigned i=0;i<4;i++) assert(aos_serial_uart_step(&uart,&io,&changed));
    assert(out_n==4 && !memcmp(output,bytes,4) && !uart.tx_pending && txq.head==4);
    assert(aos_serial_queue_write(&uart.channel.to_guest,bytes,4)==AOS_SERIAL_PUMP_OK);
    status=1;
    assert(aos_serial_uart_step(&uart,&io,&changed) && !changed && uart.rx_pending && in_n==1);
    assert(aos_serial_uart_step(&uart,&io,&changed) && !changed && in_n==1);
    uint8_t got[4]; uint32_t n;
    assert(aos_serial_queue_read(&uart.channel.to_guest,got,4,&n)==AOS_SERIAL_PUMP_OK && n==4);
    assert(!memcmp(got,bytes,4));
    status=0;
    assert(aos_serial_uart_step(&uart,&io,&changed) && changed && !uart.rx_pending);
    assert(aos_serial_queue_read(&uart.channel.to_guest,got,4,&n)==AOS_SERIAL_PUMP_OK && n==1 && got[0]==0xa1);
    io_failed=true;
    assert(!aos_serial_uart_step(&uart,&io,&changed));
    assert(!aos_serial_uart_init(&io));
    io_failed=false; status=0x02;
    assert(!aos_serial_uart_step(&uart,&io,&changed));
    status=0; txq.tail=txq.head+5;
    assert(!aos_serial_uart_step(&uart,&io,&changed));
    assert(meta.frontend_gate == 0); /* Failed I/O must release admission. */
    unsigned previous_writes = writes, previous_reads = in_n;
    uart.tx_pending = uart.rx_pending = true;
    meta.frontend_gate = AOS_SERIAL_FRONTEND_CLOSED;
    io_failed = true;
    assert(aos_serial_uart_step(&uart,&io,&changed) && !changed);
    assert(!uart.tx_pending && !uart.rx_pending && writes == previous_writes && in_n == previous_reads);
    assert(meta.frontend_gate == AOS_SERIAL_FRONTEND_CLOSED);
    puts("PASS: UART exact bytes, backpressure, I/O failures and closed frontend admission");
}
