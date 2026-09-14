extern crate alloc;

#[path = "../../libs/rust-pd/src/network.rs"]
mod network;

use network::{Client, Error, BUFFER_SIZE, CAPACITY, PAGE_BYTES};

extern "C" {
    fn native_net_test_init(page: *mut u8, bytes: usize) -> i32;
    fn native_net_test_pump(page: *mut u8, bytes: usize) -> i32;
}

#[test]
fn rust_client_exchanges_full_queues_with_production_c_pump() {
    let mut page = vec![0u64; PAGE_BYTES / 8];
    let mapping = page.as_mut_ptr().cast::<u8>();
    unsafe {
        assert_eq!(native_net_test_init(mapping, PAGE_BYTES), 0);
        let mut client = Client::from_mapping(mapping, PAGE_BYTES).unwrap();
        for round in 0..3 {
            for packet in 0..CAPACITY {
                let payload: Vec<u8> = (0..BUFFER_SIZE)
                    .map(|i| (i * 37 + packet * 13 + round) as u8).collect();
                client.send(&payload).unwrap();
            }
            assert_eq!(client.send(b"full"), Err(Error::WouldBlock));
            assert_eq!(native_net_test_pump(mapping, PAGE_BYTES), CAPACITY as i32);
            assert_eq!(client.receive(&mut [0; 16]), Err(Error::BufferTooSmall(BUFFER_SIZE)));
            for packet in 0..CAPACITY {
                let mut output = [0; BUFFER_SIZE];
                assert_eq!(client.receive(&mut output), Ok(BUFFER_SIZE));
                for (i, byte) in output.iter().enumerate() {
                    assert_eq!(*byte, (i * 37 + packet * 13 + round) as u8);
                }
            }
            assert_eq!(client.receive(&mut [0; BUFFER_SIZE]), Err(Error::WouldBlock));
        }
    }
}
