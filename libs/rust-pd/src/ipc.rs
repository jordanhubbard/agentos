//! `MsgInfo` wrapper and message-register (MR) accessors.
//!
//! The Microkit IPC ABI encodes the message descriptor in a single 64-bit word:
//!
//! ```text
//! Bits 63..12  label        (52 bits, caller-defined opcode)
//! Bits 11..9   caps_unwrapped (3 bits)
//! Bits 8..7    extra_caps   (2 bits)
//! Bits 6..0    length       (7 bits — number of MR words used)
//! ```
//!
//! This matches the seL4 / Microkit C ABI exactly so that `MsgInfo` can be
//! passed through `protected()` return values without any conversion.

/// Encoded Microkit message descriptor (`microkit_msginfo`).
///
/// The inner `u64` is the raw word as defined by the seL4 ABI.
/// All field manipulation goes through the constructor and accessors so that
/// the encoding is never scattered across call sites.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[repr(transparent)]
pub struct MsgInfo(u64);

// ── Encoding constants ────────────────────────────────────────────────────────

const LABEL_SHIFT: u64 = 12;
const LABEL_MASK: u64 = (1u64 << 52) - 1; // 52 bits

const CAPS_UNWRAPPED_SHIFT: u64 = 9;
const CAPS_UNWRAPPED_MASK: u64 = 0x7; // 3 bits

const EXTRA_CAPS_SHIFT: u64 = 7;
const EXTRA_CAPS_MASK: u64 = 0x3; // 2 bits

const LENGTH_SHIFT: u64 = 0;
const LENGTH_MASK: u64 = 0x7F; // 7 bits

impl MsgInfo {
    // ── Constructors ──────────────────────────────────────────────────────────

    /// Build a `MsgInfo` from individual fields.
    ///
    /// Values that exceed their field width are silently masked so that the
    /// encoded word always matches what the seL4 kernel would produce.
    #[inline]
    pub fn new(label: u64, caps_unwrapped: u32, extra_caps: u32, length: u32) -> Self {
        let word = ((label & LABEL_MASK) << LABEL_SHIFT)
            | (((caps_unwrapped as u64) & CAPS_UNWRAPPED_MASK) << CAPS_UNWRAPPED_SHIFT)
            | (((extra_caps as u64) & EXTRA_CAPS_MASK) << EXTRA_CAPS_SHIFT)
            | (((length as u64) & LENGTH_MASK) << LENGTH_SHIFT);
        MsgInfo(word)
    }

    /// Wrap a raw 64-bit word (as returned by the kernel) without decoding it.
    #[inline]
    pub const fn from_raw(raw: u64) -> Self {
        MsgInfo(raw)
    }

    /// Return the raw 64-bit word (for passing back into kernel syscalls).
    #[inline]
    pub const fn raw(self) -> u64 {
        self.0
    }

    // ── Field accessors ───────────────────────────────────────────────────────

    /// Caller-defined opcode / label (52-bit field).
    #[inline]
    pub fn label(self) -> u64 {
        (self.0 >> LABEL_SHIFT) & LABEL_MASK
    }

    /// Number of caps unwrapped during receive.
    #[inline]
    pub fn caps_unwrapped(self) -> u32 {
        ((self.0 >> CAPS_UNWRAPPED_SHIFT) & CAPS_UNWRAPPED_MASK) as u32
    }

    /// Number of extra cap slots transferred with the message.
    #[inline]
    pub fn extra_caps(self) -> u32 {
        ((self.0 >> EXTRA_CAPS_SHIFT) & EXTRA_CAPS_MASK) as u32
    }

    /// Number of message-register words populated in this message.
    #[inline]
    pub fn count(self) -> u32 {
        ((self.0 >> LENGTH_SHIFT) & LENGTH_MASK) as u32
    }
}

// ── Message-register accessors ────────────────────────────────────────────────
//
// In a real PD build these call into the Microkit runtime (seL4_GetMR /
// seL4_SetMR).  In unit-test builds (feature = "std") they hit the mock
// implementation provided by the test harness.

/// Read message register `idx` (0-based).
///
/// # Safety
/// Caller must ensure `idx` < 64 and that the IPC buffer is valid for the
/// current thread.  In seL4 this is always true inside `protected()` /
/// `notified()`.
#[inline]
pub fn get_mr(idx: u32) -> u64 {
    unsafe { crate::ffi::seL4_GetMR(idx as i32) }
}

/// Write message register `idx` with `val`.
///
/// # Safety
/// Same constraints as `get_mr`.
#[inline]
pub fn set_mr(idx: u32, val: u64) {
    unsafe { crate::ffi::seL4_SetMR(idx as i32, val) }
}

// ── Tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip_all_fields() {
        let info = MsgInfo::new(0xDEAD, 3, 2, 7);
        assert_eq!(info.label(), 0xDEAD);
        assert_eq!(info.caps_unwrapped(), 3);
        assert_eq!(info.extra_caps(), 2);
        assert_eq!(info.count(), 7);
    }

    #[test]
    fn label_overflow_is_masked() {
        // Only the low 52 bits of the label survive encoding.
        let full = u64::MAX;
        let info = MsgInfo::new(full, 0, 0, 0);
        assert_eq!(info.label(), LABEL_MASK);
    }

    #[test]
    fn length_overflow_is_masked() {
        // Only 7 bits of length survive.
        let info = MsgInfo::new(0, 0, 0, 0xFF);
        assert_eq!(info.count(), 0x7F);
    }

    #[test]
    fn zero_message() {
        let info = MsgInfo::new(0, 0, 0, 0);
        assert_eq!(info.raw(), 0);
        assert_eq!(info.label(), 0);
        assert_eq!(info.count(), 0);
    }

    #[test]
    fn from_raw_round_trip() {
        let original = MsgInfo::new(42, 1, 1, 3);
        let reconstructed = MsgInfo::from_raw(original.raw());
        assert_eq!(original, reconstructed);
    }

    #[test]
    fn health_check_opcode() {
        // 0xFF is the health-check opcode used by the example PD.
        let info = MsgInfo::new(0xFF, 0, 0, 0);
        assert_eq!(info.label(), 0xFF);
    }
}
