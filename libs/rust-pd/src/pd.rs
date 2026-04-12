//! Protection Domain trait and the `export_pd!` macro.
//!
//! Every Rust PD must implement the [`ProtectionDomain`] trait and then call
//! [`export_pd!`] once (at crate root) to generate the three C-compatible
//! entry points that Microkit expects:
//!
//! ```c
//! void init(void);
//! void notified(microkit_channel ch);
//! microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo);
//! ```
//!
//! # Example
//!
//! ```rust,ignore
//! use agentos_pd::pd::{ProtectionDomain, export_pd};
//! use agentos_pd::ipc::MsgInfo;
//!
//! struct MyPd;
//!
//! impl ProtectionDomain for MyPd {
//!     fn init(&mut self) { /* one-time setup */ }
//!     fn notified(&mut self, channel: u32) { /* handle async notification */ }
//!     fn protected(&mut self, channel: u32, msg: MsgInfo) -> MsgInfo {
//!         MsgInfo::new(0, 0, 0, 0) // echo back empty reply
//!     }
//! }
//!
//! static mut PD: MyPd = MyPd;
//! export_pd!(MyPd, PD);
//! ```

use crate::ipc::MsgInfo;

/// Trait that every Rust Protection Domain must implement.
///
/// Microkit calls these three entry points during the PD's lifetime:
///
/// * `init` — called once before the PD enters its event loop.
/// * `notified` — called when a notification arrives on `channel`.
/// * `protected` — called when another PD sends a protected procedure call.
///   The return value becomes the reply message.
///
/// All three are called from the Microkit runtime on the PD's single thread,
/// so no synchronisation between them is required.
pub trait ProtectionDomain {
    /// One-time initialisation.  Called by Microkit before any notifications
    /// or protected calls are delivered.
    fn init(&mut self);

    /// Handle an asynchronous notification on `channel`.
    fn notified(&mut self, channel: u32);

    /// Handle a synchronous protected procedure call arriving on `channel`.
    ///
    /// The caller's message registers are accessible via [`crate::ipc::get_mr`]
    /// before calling this function, and any MRs written with
    /// [`crate::ipc::set_mr`] before returning become the reply payload.
    /// The returned `MsgInfo` sets the label and length of the reply.
    fn protected(&mut self, channel: u32, msg: MsgInfo) -> MsgInfo;
}

/// Export a Rust `ProtectionDomain` implementation as the three C entry points
/// that seL4 Microkit expects.
///
/// # Usage
///
/// ```rust,ignore
/// static mut MY_PD_INSTANCE: MyPd = MyPd::new();
/// export_pd!(MyPd, MY_PD_INSTANCE);
/// ```
///
/// The macro generates:
///
/// ```c
/// void            init(void);
/// void            notified(microkit_channel ch);
/// microkit_msginfo protected(microkit_channel ch, microkit_msginfo info);
/// ```
///
/// # Safety
///
/// The generated functions use `unsafe` to access the static mutable instance.
/// This is sound because Microkit guarantees single-threaded delivery of all
/// entry-point calls — there is never concurrent access to the PD instance.
#[macro_export]
macro_rules! export_pd {
    ($pd_type:ty, $pd_instance:expr) => {
        /// Called by Microkit once at PD startup.
        #[no_mangle]
        pub unsafe extern "C" fn init() {
            use $crate::pd::ProtectionDomain as _;
            ($pd_instance).init();
        }

        /// Called by Microkit when a notification arrives on `ch`.
        #[no_mangle]
        pub unsafe extern "C" fn notified(ch: u32) {
            use $crate::pd::ProtectionDomain as _;
            ($pd_instance).notified(ch);
        }

        /// Called by Microkit on a protected procedure call from another PD.
        /// Returns the reply `microkit_msginfo` word.
        #[no_mangle]
        pub unsafe extern "C" fn protected(ch: u32, info: u64) -> u64 {
            use $crate::pd::ProtectionDomain as _;
            let msg = $crate::ipc::MsgInfo::from_raw(info);
            let reply = ($pd_instance).protected(ch, msg);
            reply.raw()
        }
    };
}
