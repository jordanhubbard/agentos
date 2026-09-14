//! Synchronous IPC for root-task-spawned, single-threaded Rust PDs.
//!
//! Link the C bridge, `pd_entry.o` and `sel4_crt.o`. Export a nonreturning
//! `extern "C" fn pd_main(endpoint: u64, nameserver: u64)`; the C entry point
//! initializes the IPC buffer before entering Rust. These functions use
//! endpoint capabilities directly, not Microkit channel numbers.
//!
//! A received notification must be identified by the caller's badge contract
//! and must not be replied to. The runtime does not assign notification badges.

use crate::ipc::{MsgInfo, MESSAGE_REGISTERS};

#[derive(Debug, Copy, Clone)]
pub struct Received {
    pub info: MsgInfo,
    pub badge: u64,
}

pub fn receive(endpoint: u64) -> Received {
    let mut badge = 0;
    let info = unsafe { crate::ffi::agentos_pd_receive(endpoint, &mut badge) };
    Received { info: MsgInfo::from_raw(info), badge }
}

fn validate_send(info: MsgInfo) {
    assert!(info.count() <= MESSAGE_REGISTERS, "IPC message exceeds buffer");
    assert_eq!(info.extra_caps(), 0, "capability transfer is not configured");
}

/// Reply to the most recently received synchronous call on this thread.
pub fn reply(info: MsgInfo) {
    validate_send(info);
    unsafe { crate::ffi::agentos_pd_reply(info.raw()) }
}

/// Call an endpoint capability; payload and reply use the thread's IPC buffer.
pub fn call(endpoint: u64, info: MsgInfo) -> MsgInfo {
    validate_send(info);
    MsgInfo::from_raw(unsafe { crate::ffi::agentos_pd_call(endpoint, info.raw()) })
}
