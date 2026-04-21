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

#[cfg(test)]
mod tests {
    use super::*;

    // ── ContextWindow ─────────────────────────────────────────────────────────

    #[test]
    fn context_window_starts_empty() {
        let cw = ContextWindow::new(1, 8192);
        assert_eq!(cw.used_tokens, 0);
        assert_eq!(cw.size_tokens, 8192);
        assert!(cw.snapshot_id.is_none());
    }

    #[test]
    fn context_window_fill_pct_zero_when_empty() {
        let cw = ContextWindow::new(1, 1000);
        assert!((cw.fill_pct() - 0.0).abs() < 1e-9);
    }

    #[test]
    fn context_window_fill_pct_half_full() {
        let mut cw = ContextWindow::new(1, 1000);
        cw.used_tokens = 500;
        assert!((cw.fill_pct() - 50.0).abs() < 1e-9);
    }

    #[test]
    fn context_window_fill_pct_full() {
        let mut cw = ContextWindow::new(1, 100);
        cw.used_tokens = 100;
        assert!((cw.fill_pct() - 100.0).abs() < 1e-9);
    }

    #[test]
    fn context_window_is_near_full_false_when_below_80pct() {
        let mut cw = ContextWindow::new(1, 100);
        cw.used_tokens = 79;
        assert!(!cw.is_near_full());
    }

    #[test]
    fn context_window_is_near_full_true_when_above_80pct() {
        let mut cw = ContextWindow::new(1, 100);
        cw.used_tokens = 81;
        assert!(cw.is_near_full());
    }

    // ── MemoryPressure::from_raw ──────────────────────────────────────────────

    #[test]
    fn memory_pressure_boundary_values() {
        assert_eq!(MemoryPressure::from_raw(0),   MemoryPressure::None);
        assert_eq!(MemoryPressure::from_raw(63),  MemoryPressure::None);
        assert_eq!(MemoryPressure::from_raw(64),  MemoryPressure::Low);
        assert_eq!(MemoryPressure::from_raw(127), MemoryPressure::Low);
        assert_eq!(MemoryPressure::from_raw(128), MemoryPressure::Medium);
        assert_eq!(MemoryPressure::from_raw(191), MemoryPressure::Medium);
        assert_eq!(MemoryPressure::from_raw(192), MemoryPressure::High);
        assert_eq!(MemoryPressure::from_raw(254), MemoryPressure::High);
        assert_eq!(MemoryPressure::from_raw(255), MemoryPressure::Critical);
    }

    #[test]
    fn memory_pressure_ordering() {
        assert!(MemoryPressure::Critical > MemoryPressure::High);
        assert!(MemoryPressure::High    > MemoryPressure::Medium);
        assert!(MemoryPressure::Medium  > MemoryPressure::Low);
        assert!(MemoryPressure::Low     > MemoryPressure::None);
    }

    // ── MemoryKind ────────────────────────────────────────────────────────────

    #[test]
    fn memory_kind_shared_stores_token() {
        let kind = MemoryKind::Shared { token: 0xCAFE };
        if let MemoryKind::Shared { token } = kind {
            assert_eq!(token, 0xCAFE);
        } else {
            panic!("wrong variant");
        }
    }

    #[test]
    fn memory_kind_variants_distinct() {
        assert_ne!(MemoryKind::Standard4K, MemoryKind::HugePage2M);
        assert_ne!(MemoryKind::HugePage2M, MemoryKind::HugePage1G);
    }
}
