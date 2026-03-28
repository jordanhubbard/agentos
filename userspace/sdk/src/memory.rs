//! Memory management for agents
//!
//! In agentOS, memory is a capability. You ask for it; it's either granted or not.
//! No sbrk(), no mmap() without a cap.

use alloc::string::String;

/// A memory region capability
#[derive(Debug, Clone)]
pub struct MemoryRegion {
    pub base: usize,
    pub size: usize,
    pub kind: MemoryKind,
    pub writable: bool,
    pub executable: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MemoryKind {
    Standard4K,
    HugePage2M,
    HugePage1G,
    Shared { token: u64 },
}

/// Context window snapshot - for LLM agents
///
/// This is a first-class kernel object that can be:
/// - Snapshotted to AgentFS
/// - Forked (copy-on-write)
/// - Restored (picked up where you left off)
#[derive(Debug, Clone)]
pub struct ContextWindow {
    pub id: u64,
    pub size_tokens: u64,
    pub used_tokens: u64,
    pub snapshot_id: Option<String>,
}

impl ContextWindow {
    pub fn new(id: u64, size_tokens: u64) -> Self {
        Self { id, size_tokens, used_tokens: 0, snapshot_id: None }
    }
    
    pub fn fill_pct(&self) -> f64 {
        self.used_tokens as f64 / self.size_tokens as f64 * 100.0
    }
    
    pub fn is_near_full(&self) -> bool {
        self.fill_pct() > 80.0
    }
}

/// Memory pressure levels
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum MemoryPressure {
    None = 0,
    Low = 64,
    Medium = 128,
    High = 192,
    Critical = 255,
}

impl MemoryPressure {
    pub fn from_raw(level: u8) -> Self {
        match level {
            0..=63 => Self::None,
            64..=127 => Self::Low,
            128..=191 => Self::Medium,
            192..=254 => Self::High,
            255 => Self::Critical,
        }
    }
}
