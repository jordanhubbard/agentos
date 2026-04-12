//! # agentos-pd — Rust Protection Domain runtime for seL4 Microkit
//!
//! This crate provides the building blocks for writing seL4 Microkit Protection
//! Domains (PDs) in Rust for the agentOS kernel.
//!
//! ## Quick start
//!
//! ```rust,ignore
//! #![no_std]
//! #![no_main]
//!
//! use agentos_pd::pd::ProtectionDomain;
//! use agentos_pd::ipc::MsgInfo;
//! use agentos_pd::{console, export_pd};
//!
//! struct HelloPd;
//!
//! impl ProtectionDomain for HelloPd {
//!     fn init(&mut self) {
//!         console::info("[hello] booted\n");
//!     }
//!     fn notified(&mut self, _ch: u32) {}
//!     fn protected(&mut self, _ch: u32, msg: MsgInfo) -> MsgInfo {
//!         MsgInfo::new(0, 0, 0, 0)
//!     }
//! }
//!
//! static mut PD: HelloPd = HelloPd;
//! export_pd!(HelloPd, PD);
//! ```
//!
//! ## Modules
//!
//! * [`ffi`] — raw `extern "C"` bindings to the Microkit runtime and seL4 IPC.
//! * [`ipc`] — safe [`MsgInfo`](ipc::MsgInfo) wrapper and MR accessors.
//! * [`pd`]  — the [`ProtectionDomain`](pd::ProtectionDomain) trait and the
//!   [`export_pd!`] macro that generates the C entry points.
//! * [`console`] — structured logging via `console_log`.

// In real PD builds this crate is `no_std`.  The "std" feature is only enabled
// for host-side unit testing (see tests/rust_pd_unit_test.rs).
#![cfg_attr(not(feature = "std"), no_std)]

pub mod console;
pub mod ffi;
pub mod ipc;
pub mod pd;

// `export_pd!` is automatically available at the crate root via `#[macro_export]`
// in pd.rs; no explicit re-export is needed.

/// Minimal panic handler for `no_std` PD builds.
///
/// In a real seL4 PD the right behaviour on panic is to spin-halt or call the
/// Microkit fault handler.  We use an infinite loop here as a safe default.
/// Code compiled with `--features std` (host testing) uses the std panic runtime
/// instead and does NOT compile this symbol.
#[cfg(not(feature = "std"))]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

/// agentOS PD runtime version string.
pub const PD_RUNTIME_VERSION: &str = env!("CARGO_PKG_VERSION");

/// ABI version of the Microkit entry-point protocol this crate targets.
/// Bump whenever the `init` / `notified` / `protected` signatures change.
pub const MICROKIT_ABI_VERSION: u32 = 1;
