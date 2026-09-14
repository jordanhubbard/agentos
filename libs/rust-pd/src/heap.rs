//! Fixed-capacity heap backed by private, pre-mapped PD storage.
//!
//! Allocation scans at most BLOCKS metadata entries, rounding sizes to 64-byte
//! blocks. Freeing an allocation restores those entries, including adjacent
//! free spans. There are no page requests, IPC calls or allocations inside the
//! allocator. A spin lock makes metadata access thread-safe; the current PD
//! runtime has one thread and must not reenter allocation from an interrupt.
//! This bounds memory and search work, not lock wait time under contention.

use core::{
    alloc::{GlobalAlloc, Layout},
    cell::UnsafeCell,
    ptr,
    sync::atomic::{AtomicBool, Ordering},
};

pub const BLOCK_BYTES: usize = 64;

#[derive(Clone, Copy)]
#[repr(C, align(64))]
struct Block([u8; BLOCK_BYTES]);

/// Use as a static global allocator. Its address must remain fixed while
/// allocations are live; storage belongs only to the containing PD.
pub struct BoundedHeap<const BLOCKS: usize> {
    locked: AtomicBool,
    // Zero is free, an allocation head stores its length, and continuation
    // blocks hold usize::MAX. Metadata is never stored in user payload bytes.
    lengths: UnsafeCell<[usize; BLOCKS]>,
    storage: UnsafeCell<[Block; BLOCKS]>,
}

// The lock serializes metadata. Distinct live allocations cannot overlap, and
// payload storage is touched only by the caller holding that allocation.
unsafe impl<const BLOCKS: usize> Sync for BoundedHeap<BLOCKS> {}

struct Guard<'a>(&'a AtomicBool);
impl Drop for Guard<'_> {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}

impl<const BLOCKS: usize> BoundedHeap<BLOCKS> {
    pub const fn new() -> Self {
        Self {
            locked: AtomicBool::new(false),
            lengths: UnsafeCell::new([0; BLOCKS]),
            storage: UnsafeCell::new([Block([0; BLOCK_BYTES]); BLOCKS]),
        }
    }

    pub const fn capacity_bytes(&self) -> usize {
        BLOCKS * BLOCK_BYTES
    }

    fn lock(&self) -> Guard<'_> {
        while self
            .locked
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        Guard(&self.locked)
    }

    fn required(layout: Layout) -> Option<usize> {
        let size = layout.size();
        if size == 0 {
            return None;
        }
        let blocks = size.checked_add(BLOCK_BYTES - 1)? / BLOCK_BYTES;
        if blocks > BLOCKS {
            None
        } else {
            Some(blocks)
        }
    }
}

impl<const BLOCKS: usize> Default for BoundedHeap<BLOCKS> {
    fn default() -> Self {
        Self::new()
    }
}

unsafe impl<const BLOCKS: usize> GlobalAlloc for BoundedHeap<BLOCKS> {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let Some(needed) = Self::required(layout) else {
            return ptr::null_mut();
        };
        let _guard = self.lock();
        let lengths = &mut *self.lengths.get();
        let base = self.storage.get().cast::<u8>();
        let mut start = 0;
        let mut run = 0;
        for index in 0..BLOCKS {
            if lengths[index] != 0 {
                run = 0;
                continue;
            }
            if run == 0 {
                let candidate = base.add(index * BLOCK_BYTES);
                if candidate as usize & (layout.align() - 1) != 0 {
                    continue;
                }
                start = index;
            }
            run += 1;
            if run == needed {
                lengths[start] = needed;
                for entry in &mut lengths[start + 1..start + needed] {
                    *entry = usize::MAX;
                }
                return base.add(start * BLOCK_BYTES);
            }
        }
        ptr::null_mut()
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        let Some(needed) = Self::required(layout) else {
            return;
        };
        let base = self.storage.get().cast::<u8>() as usize;
        let Some(offset) = (pointer as usize).checked_sub(base) else {
            return;
        };
        if offset >= self.capacity_bytes()
            || offset % BLOCK_BYTES != 0
            || pointer as usize & (layout.align() - 1) != 0
        {
            return;
        }
        let start = offset / BLOCK_BYTES;
        if needed > BLOCKS - start {
            return;
        }
        let _guard = self.lock();
        let lengths = &mut *self.lengths.get();
        if lengths[start] != needed {
            return;
        }
        lengths[start..start + needed].fill(0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn varied_sizes_keep_distinct_payloads() {
        let heap = BoundedHeap::<128>::new();
        let mut live = std::vec::Vec::new();
        for (tag, size) in [1, 63, 64, 65, 511].into_iter().enumerate() {
            let layout = Layout::from_size_align(size, 32).unwrap();
            let p = unsafe { heap.alloc(layout) };
            assert!(!p.is_null());
            assert_eq!(p as usize % 32, 0);
            unsafe {
                p.write_bytes(tag as u8, size);
            }
            live.push((p, layout, tag as u8));
        }
        for (p, layout, tag) in live {
            assert!(unsafe { core::slice::from_raw_parts(p, layout.size()) }
                .iter()
                .all(|byte| *byte == tag));
            unsafe {
                heap.dealloc(p, layout);
            }
        }
    }

    #[test]
    fn page_alignment_and_zeroed_reuse() {
        let heap = BoundedHeap::<128>::new();
        for alignment in [1, 64, 128, 512, 4096] {
            let layout = Layout::from_size_align(257, alignment).unwrap();
            let p = unsafe { heap.alloc(layout) };
            assert!(!p.is_null());
            assert_eq!(p as usize % alignment, 0);
            unsafe {
                p.write_bytes(0xa5, 257);
                heap.dealloc(p, layout);
            }
            let zero = unsafe { heap.alloc_zeroed(layout) };
            assert!(!zero.is_null());
            assert!(unsafe { core::slice::from_raw_parts(zero, 257) }
                .iter()
                .all(|byte| *byte == 0));
            unsafe {
                heap.dealloc(zero, layout);
            }
        }
    }

    #[test]
    fn fragmentation_exhaustion_and_complete_reuse() {
        let heap = BoundedHeap::<64>::new();
        let chunk = Layout::from_size_align(512, 64).unwrap();
        let mut pointers = [ptr::null_mut(); 8];
        for p in &mut pointers {
            *p = unsafe { heap.alloc(chunk) };
            assert!(!p.is_null());
        }
        assert!(unsafe { heap.alloc(chunk) }.is_null());
        for i in [0, 2, 4, 6] {
            unsafe {
                heap.dealloc(pointers[i], chunk);
            }
        }
        assert!(unsafe { heap.alloc(Layout::from_size_align(1024, 64).unwrap()) }.is_null());
        for i in [1, 3, 5, 7] {
            unsafe {
                heap.dealloc(pointers[i], chunk);
            }
        }
        let full = Layout::from_size_align(heap.capacity_bytes(), 64).unwrap();
        for _ in 0..3 {
            let p = unsafe { heap.alloc(full) };
            assert_eq!(p, pointers[0]);
            unsafe {
                heap.dealloc(p, full);
            }
        }
    }

    #[test]
    fn realloc_preserves_bytes_and_failure_preserves_old_allocation() {
        let heap = BoundedHeap::<64>::new();
        let small = Layout::from_size_align(128, 64).unwrap();
        let p = unsafe { heap.alloc(small) };
        assert!(!p.is_null());
        unsafe {
            p.write_bytes(0x37, 128);
        }
        let larger = unsafe { heap.realloc(p, small, 1024) };
        assert!(!larger.is_null());
        assert!(unsafe { core::slice::from_raw_parts(larger, 128) }
            .iter()
            .all(|byte| *byte == 0x37));
        let layout = Layout::from_size_align(1024, 64).unwrap();
        assert!(unsafe { heap.realloc(larger, layout, 8192) }.is_null());
        assert!(unsafe { core::slice::from_raw_parts(larger, 128) }
            .iter()
            .all(|byte| *byte == 0x37));
        unsafe {
            heap.dealloc(larger, layout);
        }
    }

    #[test]
    fn concurrent_allocations_keep_payloads_private() {
        let heap = std::sync::Arc::new(BoundedHeap::<128>::new());
        let mut threads = std::vec::Vec::new();
        for tag in 1..=4 {
            let heap = heap.clone();
            threads.push(std::thread::spawn(move || {
                let layout = Layout::from_size_align(256, 64).unwrap();
                for _ in 0..100 {
                    let p = unsafe { heap.alloc(layout) };
                    assert!(!p.is_null());
                    unsafe {
                        p.write_bytes(tag, 256);
                    }
                    std::thread::yield_now();
                    assert!(unsafe { core::slice::from_raw_parts(p, 256) }
                        .iter()
                        .all(|byte| *byte == tag));
                    unsafe {
                        heap.dealloc(p, layout);
                    }
                }
            }));
        }
        for thread in threads {
            thread.join().unwrap();
        }
        let full = Layout::from_size_align(heap.capacity_bytes(), 64).unwrap();
        let p = unsafe { heap.alloc(full) };
        assert!(!p.is_null());
        unsafe {
            heap.dealloc(p, full);
        }
    }
}
