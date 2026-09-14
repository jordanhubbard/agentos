#![no_std]
//! Target implementation of contracts/native_rust_probe.h.
use agentos_pd::{
    ipc::{get_mr, set_mr, MsgInfo},
    runtime,
};
extern crate alloc;
use agentos_pd::executor::{Executor, SpawnFailure};
use agentos_pd::heap::BoundedHeap;
use alloc::vec::Vec;
use alloc::{boxed::Box, sync::Arc};
use core::alloc::{GlobalAlloc, Layout};
use core::{
    future::Future,
    pin::Pin,
    sync::atomic::{AtomicU64, Ordering},
    task::{Context, Poll},
};

#[global_allocator]
static HEAP: BoundedHeap<1024> = BoundedHeap::new();

struct YieldTwice(u8);
impl Future for YieldTwice {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if self.0 == 0 {
            return Poll::Ready(());
        }
        self.0 -= 1;
        cx.waker().wake_by_ref();
        Poll::Pending
    }
}

async fn count_after_yield(done: Arc<AtomicU64>) {
    YieldTwice(2).await;
    done.fetch_add(1, Ordering::Relaxed);
}

fn executor_proof() -> Option<[u64; 8]> {
    let mut executor = Executor::<2>::new();
    let done = Arc::new(AtomicU64::new(0));
    executor
        .spawn(Box::pin(count_after_yield(done.clone())))
        .ok()?;
    executor
        .spawn(Box::pin(count_after_yield(done.clone())))
        .ok()?;
    let rejected = executor
        .spawn(Box::pin(count_after_yield(done.clone())))
        .err()?;
    if rejected.reason != SpawnFailure::Full {
        return None;
    }
    drop(rejected);
    if executor.run_ready(0).polls != 0 || done.load(Ordering::Relaxed) != 0 {
        return None;
    }
    let mut polls = 0;
    for _ in 0..2 {
        let stats = executor.run_ready(1);
        if stats.polls != 1 || stats.completed != 0 {
            return None;
        }
        polls += stats.polls;
    }
    let stats = executor.run_ready(usize::MAX);
    if stats.polls != 2 || stats.completed != 0 {
        return None;
    }
    polls += stats.polls;
    for _ in 0..2 {
        let stats = executor.run_ready(1);
        if stats.polls != 1 || stats.completed != 1 {
            return None;
        }
        polls += stats.polls;
    }
    let completed = done.load(Ordering::Relaxed);
    if completed != 2 || !executor.is_empty() {
        return None;
    }
    let cancelled = executor.spawn(Box::pin(core::future::pending())).ok()?;
    if !executor.cancel(cancelled) {
        return None;
    }
    let final_done = done.clone();
    executor
        .spawn(Box::pin(async move {
            final_done.fetch_add(1, Ordering::Relaxed);
        }))
        .ok()?;
    if executor.cancel(cancelled) {
        return None;
    }
    let stats = executor.run_ready(1);
    if stats.completed != 1 || stats.remaining != 0 {
        return None;
    }
    Some([
        1,
        polls as u64,
        completed,
        1,
        0,
        1,
        done.load(Ordering::Relaxed),
        0,
    ])
}

fn heap_proof(seed: u8) -> Option<u64> {
    let mut bytes = Vec::new();
    bytes.try_reserve_exact(2049).ok()?;
    for i in 0..2049 {
        bytes.push((i * 37 + usize::from(seed)) as u8);
    }
    let checksum = core::hint::black_box(bytes.as_slice())
        .iter()
        .fold(0u64, |sum, byte| sum.rotate_left(5) ^ u64::from(*byte));
    drop(bytes);
    let aligned = Layout::from_size_align(128, 4096).ok()?;
    let pointer = unsafe { HEAP.alloc(aligned) };
    if pointer.is_null() {
        return None;
    }
    let remainder = pointer as usize % 4096;
    unsafe {
        pointer.write_volatile(seed);
        HEAP.dealloc(pointer, aligned);
    }
    if remainder != 0 {
        return None;
    }

    let full = Layout::from_size_align(HEAP.capacity_bytes(), 64).ok()?;
    let one = Layout::from_size_align(1, 1).ok()?;
    for _ in 0..2 {
        let whole = unsafe { HEAP.alloc(full) };
        if whole.is_null() {
            return None;
        }
        let extra = unsafe { HEAP.alloc(one) };
        unsafe {
            HEAP.dealloc(whole, full);
        }
        if !extra.is_null() {
            unsafe {
                HEAP.dealloc(extra, one);
            }
            return None;
        }
    }
    Some(checksum)
}

fn network_proof(queue: &mut agentos_pd::network::Client<'_>,
    attachment: agentos_pd::network::Attachment) -> Option<[u64; 3]> {
    use agentos_pd::network;
    if attachment.hardware != 1 { return None; }
    let mut wakes = 0;
    for _ in 0..3u8 {
        // The driver assigns 10.0.2.(15 + client_id), not arbitrary aliases.
        let ip = [10, 0, 2, 17];
        let mut arp = [0u8; 42];
        arp[..6].fill(255);
        arp[6..12].copy_from_slice(&attachment.mac);
        arp[12..22].copy_from_slice(&[8, 6, 0, 1, 8, 0, 6, 4, 0, 1]);
        arp[22..28].copy_from_slice(&attachment.mac);
        arp[28..32].copy_from_slice(&ip);
        arp[38..42].copy_from_slice(&[10, 0, 2, 2]);
        queue.send(&arp).ok()?;
        network::signal(22);
        let mut matched = false;
        for _ in 0..64 {
            if network::wait(24) != 0x40000000 { return None; }
            wakes += 1;
            for _ in 0..network::CAPACITY {
                let mut packet = [0; network::BUFFER_SIZE];
                let len = match queue.receive(&mut packet) {
                    Ok(len) => len,
                    Err(network::Error::WouldBlock) => break,
                    Err(_) => return None,
                };
                if len >= 42 && packet[..6] == attachment.mac &&
                    packet[12..22] == [8, 6, 0, 1, 8, 0, 6, 4, 0, 2] &&
                    packet[6..12] == packet[22..28] &&
                    packet[28..32] == [10, 0, 2, 2] &&
                    packet[32..38] == attachment.mac && packet[38..42] == ip {
                    matched = true;
                }
            }
            if queue.needs_kick().ok()? { network::signal(22); }
            if matched { break; }
        }
        if !matched { return None; }
    }
    Some([attachment.hardware as u64, 3, wakes])
}

#[no_mangle]
pub extern "C" fn pd_main(endpoint: u64, _nameserver: u64) -> ! {
    let mut network = unsafe {
        agentos_pd::network::initialize_and_attach(0x26400000usize as *mut u8,
            agentos_pd::network::PAGE_BYTES, 15, 2, 2).ok()
    };
    let network_result = network.as_mut().and_then(|(queue, attachment)| network_proof(queue, *attachment));
    let mut network_sequence = 0;
    extern "C" { fn agentos_pd_net_isolation_probe(); }
    // Enabled only in a dedicated probe image, after proving the owned path.
    if network_result.is_some() { unsafe { agentos_pd_net_isolation_probe() }; }
    loop {
        let request = runtime::receive(endpoint);
        if request.badge == 0x40000000 { continue; }
        let (status, count) = match request.info.label() {
            0x2e05 if request.info.count() != 1 => (2, 0),
            0x2e05 if get_mr(0) != 1 => (3, 0),
            0x2e05 => match network.as_mut().and_then(|(queue, attachment)| network_proof(queue, *attachment)) {
                Some(words) => {
                    for (index, value) in words.iter().enumerate() { set_mr(index as u32, *value); }
                    network_sequence += 1;
                    set_mr(3, network_sequence);
                    (0, 4)
                }
                None => (6, 0),
            },
            0x2e04 if request.info.count() != 1 => (2, 0),
            0x2e04 if get_mr(0) != 1 => (3, 0),
            0x2e04 => match executor_proof() {
                Some(words) => {
                    for (index, value) in words.iter().enumerate() {
                        set_mr(index as u32, *value);
                    }
                    (0, 8)
                }
                None => (5, 0),
            },
            0x2e03 if request.info.count() != 2 => (2, 0),
            0x2e03 if get_mr(0) != 1 => (3, 0),
            0x2e03 if get_mr(1) > 255 => (4, 0),
            0x2e03 => match heap_proof(get_mr(1) as u8) {
                Some(checksum) => {
                    for (index, value) in [1, 2049, checksum, 0, 1, 1].iter().enumerate() {
                        set_mr(index as u32, *value);
                    }
                    (0, 6)
                }
                None => (4, 0),
            },
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
