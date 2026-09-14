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

#[no_mangle]
pub extern "C" fn pd_main(endpoint: u64, _nameserver: u64) -> ! {
    loop {
        let request = runtime::receive(endpoint);
        let (status, count) = match request.info.label() {
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
