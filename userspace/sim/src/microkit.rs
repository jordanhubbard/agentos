//! Mock seL4 Microkit shim.
//!
//! Provides the same interface as the real Microkit runtime but runs entirely
//! on the host.  The mock captures all IPC and notification traffic so tests
//! can assert on what the agent communicated.

use std::collections::{HashMap, VecDeque};

/// Number of message registers (matches Microkit ABI)
pub const MR_COUNT: usize = 8;

/// seL4 message info: label (opcode) + count of valid MRs.
#[derive(Debug, Clone, Copy, Default)]
pub struct MsgInfo {
    pub label:    u64,
    pub mr_count: u8,
}

impl MsgInfo {
    pub fn new(label: u64, mr_count: u8) -> Self {
        Self { label, mr_count }
    }
}

/// One IPC call captured by the shim.
#[derive(Debug, Clone)]
pub struct CapturedCall {
    pub channel: u32,
    pub info:    MsgInfo,
    pub regs:    [u64; MR_COUNT],
}

/// Pending notification bitmask entry.
#[derive(Debug, Clone)]
pub struct CapturedNotify {
    pub channel: u32,
}

/// Return value from a simulated ppcall.
#[derive(Debug, Clone, Default)]
pub struct PpcResult {
    pub info: MsgInfo,
    pub regs: [u64; MR_COUNT],
}

/// Registered ppcall handler: given a call, return a result.
pub type PpcHandler = Box<dyn Fn(&CapturedCall) -> PpcResult + Send + Sync>;

/// The mock Microkit runtime for one Protection Domain.
pub struct MicrokitShim {
    /// Message registers (shared between caller and callee)
    pub regs: [u64; MR_COUNT],

    /// Captured outgoing ppcall log (in order)
    pub call_log: Vec<CapturedCall>,

    /// Captured outgoing notify log
    pub notify_log: Vec<CapturedNotify>,

    /// Pending incoming notifications (bitmask, one per channel 0..63)
    pub pending_notifs: u64,

    /// Per-channel ppcall handlers (optional; default returns zeros)
    pub handlers: HashMap<u32, PpcHandler>,

    /// Per-channel reply queues (for agents that act as servers)
    pub reply_queues: HashMap<u32, VecDeque<PpcResult>>,
}

impl MicrokitShim {
    pub fn new() -> Self {
        Self {
            regs:           [0u64; MR_COUNT],
            call_log:       Vec::new(),
            notify_log:     Vec::new(),
            pending_notifs: 0,
            handlers:       HashMap::new(),
            reply_queues:   HashMap::new(),
        }
    }

    /// Set a message register.
    pub fn mr_set(&mut self, mr: usize, value: u64) {
        if mr < MR_COUNT { self.regs[mr] = value; }
    }

    /// Get a message register.
    pub fn mr_get(&self, mr: usize) -> u64 {
        if mr < MR_COUNT { self.regs[mr] } else { 0 }
    }

    /// Simulate `microkit_ppcall(channel, msginfo)`.
    ///
    /// Records the call, invokes any registered handler, writes the reply
    /// into the message registers and returns the reply `MsgInfo`.
    pub fn ppcall(&mut self, channel: u32, info: MsgInfo) -> MsgInfo {
        let call = CapturedCall { channel, info, regs: self.regs };
        self.call_log.push(call.clone());

        // Try static handler first, then reply queue, then zero reply.
        let result = if let Some(h) = self.handlers.get(&channel) {
            h(&call)
        } else if let Some(q) = self.reply_queues.get_mut(&channel) {
            q.pop_front().unwrap_or_default()
        } else {
            PpcResult::default()
        };

        self.regs = result.regs;
        result.info
    }

    /// Simulate `microkit_notify(channel)`.
    pub fn notify(&mut self, channel: u32) {
        self.notify_log.push(CapturedNotify { channel });
    }

    /// Queue an incoming notification on `channel` (drives `notified(ch)` calls).
    pub fn inject_notification(&mut self, channel: u32) {
        if channel < 64 {
            self.pending_notifs |= 1u64 << channel;
        }
    }

    /// Pop one pending notification, returning its channel if any.
    pub fn pop_notification(&mut self) -> Option<u32> {
        if self.pending_notifs == 0 { return None; }
        let ch = self.pending_notifs.trailing_zeros() as u32;
        self.pending_notifs &= !(1u64 << ch);
        Some(ch)
    }

    /// Register a handler for a given channel's ppcall.
    pub fn on_ppcall<F>(&mut self, channel: u32, handler: F)
    where F: Fn(&CapturedCall) -> PpcResult + Send + Sync + 'static,
    {
        self.handlers.insert(channel, Box::new(handler));
    }

    /// Enqueue a canned reply for the next ppcall on `channel`.
    pub fn enqueue_reply(&mut self, channel: u32, result: PpcResult) {
        self.reply_queues.entry(channel).or_default().push_back(result);
    }
}

impl Default for MicrokitShim {
    fn default() -> Self { Self::new() }
}
