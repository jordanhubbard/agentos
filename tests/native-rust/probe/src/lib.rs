#![no_std]
//! Target implementation of contracts/native_rust_probe.h.
use agentos_pd::{ipc::{get_mr, set_mr, MsgInfo}, runtime};

#[no_mangle]
pub extern "C" fn pd_main(endpoint: u64, _nameserver: u64) -> ! {
    loop {
        let request = runtime::receive(endpoint);
        let (status, count) = match request.info.label() {
            0x2e02 if request.info.count() == 0 => {
                set_mr(0, request.badge);
                (0, 1)
            }
            0x2e01 if request.info.count() != 120 => (2, 0),
            0x2e01 if get_mr(0) != 1 => (3, 0),
            0x2e01 => {
                for i in 1..120 {
                    set_mr(i, get_mr(i) ^ (0x5a5a5a5a5a5a5a5a + u64::from(i)));
                }
                (0, 120)
            }
            _ => (1, 0),
        };
        runtime::reply(MsgInfo::new(status, 0, 0, count));
    }
}
