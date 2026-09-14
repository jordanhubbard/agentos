//! Copy-based client of the platform/net_layout.h sDDF queue ABI.
//!
//! This module neither attaches to net_virt nor obtains device authority.
//! The embedding PD must provision one isolated client page, attach through
//! its root-minted capability, and deliver/retry queue notifications.

use core::{marker::PhantomData, ptr, sync::atomic::{fence, Ordering}};

pub const BUFFER_SIZE: usize = 2048;
pub const CAPACITY: usize = 32;
pub const PAGE_BYTES: usize = 0x200000;
const RX_FREE: usize = 0;
const RX_ACTIVE: usize = 0x1000;
const TX_FREE: usize = 0x2000;
const TX_ACTIVE: usize = 0x3000;
const RX_DATA: usize = 0x4000;
const TX_DATA: usize = RX_DATA + CAPACITY * BUFFER_SIZE;

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Descriptor { offset: u64, len: u16, pad0: u16, pad1: u32 }

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error { InvalidMapping, InvalidLength, CorruptQueue, InvalidDescriptor, WouldBlock, BufferTooSmall(usize) }

/// One exclusively owned client-side endpoint; not Send or Sync.
pub struct Client<'a> {
    page: *mut u8,
    _mapping: PhantomData<&'a mut [u8]>,
    _single_thread: PhantomData<*mut ()>,
}

impl<'a> Client<'a> {
    /// # Safety
    /// `page` must remain a mapped, initialized, readable/writable client page
    /// for `'a`. It must be aligned to eight bytes and have at least PAGE_BYTES
    /// bytes. The caller exclusively owns the client side of all four SPSC
    /// queues. Only the external virtualizer may concurrently access the peer
    /// sides, following the platform queue ownership protocol. No Rust references
    /// may alias the shared bytes. The client must not be used before attach.
    pub unsafe fn from_mapping(page: *mut u8, bytes: usize) -> Result<Self, Error> {
        if page.is_null() || (page as usize) % 8 != 0 || bytes < PAGE_BYTES {
            return Err(Error::InvalidMapping);
        }
        Ok(Self { page, _mapping: PhantomData, _single_thread: PhantomData })
    }

    // Snapshot indices before using them. Never trust a shared capacity or a
    // second read of an index to bound a descriptor access.
    unsafe fn indices(&self, queue: usize) -> Result<(u16, u16), Error> {
        let tail = ptr::read_volatile(self.page.add(queue).cast::<u16>());
        let head = ptr::read_volatile(self.page.add(queue + 2).cast::<u16>());
        fence(Ordering::SeqCst);
        if tail.wrapping_sub(head) as usize > CAPACITY { return Err(Error::CorruptQueue); }
        Ok((head, tail))
    }

    unsafe fn descriptor(&self, queue: usize, index: u16) -> *mut Descriptor {
        self.page.add(queue + 8 + (index as usize % CAPACITY) * 16).cast()
    }

    unsafe fn publish(&mut self, queue: usize, tail: u16, descriptor: Descriptor) {
        ptr::write_volatile(self.descriptor(queue, tail), descriptor);
        fence(Ordering::SeqCst);
        ptr::write_volatile(self.page.add(queue).cast::<u16>(), tail.wrapping_add(1));
        fence(Ordering::SeqCst);
    }

    unsafe fn consume(&mut self, queue: usize, head: u16) {
        fence(Ordering::SeqCst);
        ptr::write_volatile(self.page.add(queue + 2).cast::<u16>(), head.wrapping_add(1));
        fence(Ordering::SeqCst);
    }

    fn offset(descriptor: Descriptor) -> Result<usize, Error> {
        if descriptor.offset >= (CAPACITY * BUFFER_SIZE) as u64
            || descriptor.offset % BUFFER_SIZE as u64 != 0 {
            return Err(Error::InvalidDescriptor);
        }
        Ok(descriptor.offset as usize)
    }

    /// Copy one packet and publish it. WouldBlock leaves queues unchanged.
    pub fn send(&mut self, packet: &[u8]) -> Result<(), Error> {
        if packet.is_empty() || packet.len() > BUFFER_SIZE { return Err(Error::InvalidLength); }
        unsafe {
            let (free_head, free_tail) = self.indices(TX_FREE)?;
            let (active_head, active_tail) = self.indices(TX_ACTIVE)?;
            if free_head == free_tail || active_tail.wrapping_sub(active_head) as usize == CAPACITY {
                return Err(Error::WouldBlock);
            }
            let mut descriptor = ptr::read_volatile(self.descriptor(TX_FREE, free_head));
            let offset = Self::offset(descriptor)?;
            for (i, byte) in packet.iter().enumerate() {
                ptr::write_volatile(self.page.add(TX_DATA + offset + i), *byte);
            }
            descriptor.len = packet.len() as u16;
            descriptor.pad0 = 0;
            descriptor.pad1 = 0;
            self.consume(TX_FREE, free_head);
            self.publish(TX_ACTIVE, active_tail, descriptor);
        }
        Ok(())
    }

    /// Copy a packet into private memory and recycle its buffer. A short
    /// destination returns the required length without consuming the packet.
    pub fn receive(&mut self, output: &mut [u8]) -> Result<usize, Error> {
        unsafe {
            let (active_head, active_tail) = self.indices(RX_ACTIVE)?;
            let (free_head, free_tail) = self.indices(RX_FREE)?;
            if active_head == active_tail || free_tail.wrapping_sub(free_head) as usize == CAPACITY {
                return Err(Error::WouldBlock);
            }
            let descriptor = ptr::read_volatile(self.descriptor(RX_ACTIVE, active_head));
            let offset = Self::offset(descriptor)?;
            let len = descriptor.len as usize;
            if len == 0 || len > BUFFER_SIZE { return Err(Error::InvalidDescriptor); }
            if output.len() < len { return Err(Error::BufferTooSmall(len)); }
            for (i, byte) in output[..len].iter_mut().enumerate() {
                *byte = ptr::read_volatile(self.page.add(RX_DATA + offset + i));
            }
            self.consume(RX_ACTIVE, active_head);
            self.publish(RX_FREE, free_tail, Descriptor { len: 0, pad0: 0, pad1: 0, ..descriptor });
            Ok(len)
        }
    }

    /// Whether net_virt requested a kick for published TX or recycled RX.
    /// The caller must retry a dropped endpoint kick; this is not a wakeup.
    pub fn needs_kick(&self) -> Result<bool, Error> {
        unsafe {
            for queue in [TX_ACTIVE, RX_FREE] {
                let (head, tail) = self.indices(queue)?;
                if head != tail && ptr::read_volatile(self.page.add(queue + 4).cast::<u32>()) == 0 {
                    return Ok(true);
                }
            }
        }
        Ok(false)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    fn page() -> alloc::vec::Vec<u64> { vec![0; PAGE_BYTES / 8] }
    unsafe fn fixture(page: *mut u8, queue: usize, head: u16, descriptors: &[Descriptor]) {
        ptr::write(page.add(queue).cast::<u16>(), head.wrapping_add(descriptors.len() as u16));
        ptr::write(page.add(queue + 2).cast::<u16>(), head);
        for (i, descriptor) in descriptors.iter().enumerate() {
            ptr::write(page.add(queue + 8 + (head.wrapping_add(i as u16) as usize % CAPACITY) * 16).cast(), *descriptor);
        }
    }

    #[test]
    fn packet_copy_recycling_and_index_wrap() {
        let mut storage = page(); let p = storage.as_mut_ptr().cast::<u8>();
        unsafe {
            fixture(p, TX_FREE, u16::MAX, &[Descriptor::default()]);
            fixture(p, TX_ACTIVE, u16::MAX, &[]);
            let mut client = Client::from_mapping(p, PAGE_BYTES).unwrap();
            client.send(b"native packet").unwrap();
            assert_eq!(client.indices(TX_FREE), Ok((0, 0)));
            assert_eq!(client.indices(TX_ACTIVE), Ok((u16::MAX, 0)));
            let sent = ptr::read(client.descriptor(TX_ACTIVE, u16::MAX));
            assert_eq!(sent.len, 13);
            assert_eq!(core::slice::from_raw_parts(p.add(TX_DATA), 13), b"native packet");
            ptr::copy_nonoverlapping(p.add(TX_DATA), p.add(RX_DATA), 13);
            fixture(p, RX_ACTIVE, u16::MAX, &[sent]);
            fixture(p, RX_FREE, u16::MAX, &[]);
            assert_eq!(client.receive(&mut [0; 12]), Err(Error::BufferTooSmall(13)));
            let mut output = [0; 13];
            assert_eq!(client.receive(&mut output), Ok(13));
            assert_eq!(&output, b"native packet");
            assert_eq!(client.indices(RX_ACTIVE), Ok((0, 0)));
            assert_eq!(client.indices(RX_FREE), Ok((u16::MAX, 0)));
            assert_eq!(ptr::read(client.descriptor(RX_FREE, u16::MAX)).len, 0);
            assert!(client.needs_kick().unwrap());
        }
    }

    #[test]
    fn corruption_and_backpressure_preserve_private_output_and_indices() {
        let mut storage = page(); let p = storage.as_mut_ptr().cast::<u8>();
        unsafe {
            let mut client = Client::from_mapping(p, PAGE_BYTES).unwrap();
            assert_eq!(client.send(b"x"), Err(Error::WouldBlock));
            fixture(p, TX_FREE, 0, &[Descriptor { offset: u64::MAX, ..Descriptor::default() }]);
            assert_eq!(client.send(b"x"), Err(Error::InvalidDescriptor));
            assert_eq!(client.indices(TX_FREE), Ok((0, 1)));
            fixture(p, RX_ACTIVE, 0, &[Descriptor { len: 2049, ..Descriptor::default() }]);
            let mut output = [0xa5; BUFFER_SIZE];
            assert_eq!(client.receive(&mut output), Err(Error::InvalidDescriptor));
            assert!(output.iter().all(|b| *b == 0xa5));
            assert_eq!(client.indices(RX_ACTIVE), Ok((0, 1)));
            ptr::write(p.add(RX_ACTIVE).cast::<u16>(), 33);
            assert_eq!(client.receive(&mut output), Err(Error::CorruptQueue));
            assert_eq!(client.send(&[]), Err(Error::InvalidLength));
            assert_eq!(client.send(&[0; BUFFER_SIZE + 1]), Err(Error::InvalidLength));
        }
    }
}
