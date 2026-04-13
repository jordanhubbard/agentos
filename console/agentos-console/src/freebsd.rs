//! FreeBSD asset download state.
//!
//! Tracks the phase of any in-progress FreeBSD asset preparation so that
//! the bridge API can report status.

use serde::{Deserialize, Serialize};
use std::sync::{Arc, Mutex};

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum FreeBsdPhase {
    Idle,
    Preparing,
    Running,
    Error,
}

/// An extra device attached to the VM (beyond the boot disk and primary NIC).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct VmExtraDevice {
    /// Unique id within this VM (e.g. "disk1", "nic1").
    pub id:      String,
    /// "disk" or "nic"
    #[serde(rename = "type")]
    pub kind:    String,
    /// Human-readable label.
    pub label:   String,
    /// Path to disk image (disks only).
    pub path:    Option<String>,
    /// Host port for NIC hostfwd (NICs only, e.g. 2224).
    pub port:    Option<u16>,
    /// Whether the device is live-attached (true) or queued for next boot.
    pub live:    bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct FreeBsdState {
    pub phase:    FreeBsdPhase,
    pub step:     String,
    pub progress: u8,   // 0–100
    pub error:    Option<String>,
    pub qemu_pid: Option<u32>,
    /// Extra devices beyond the boot disk and primary NIC.
    pub devices:  Vec<VmExtraDevice>,
    /// Path to the QEMU QMP socket (set while QEMU is running).
    pub qmp_sock: Option<String>,
}

impl Default for FreeBsdState {
    fn default() -> Self {
        FreeBsdState {
            phase:    FreeBsdPhase::Idle,
            step:     String::new(),
            progress: 0,
            error:    None,
            qemu_pid: None,
            devices:  Vec::new(),
            qmp_sock: None,
        }
    }
}

pub type SharedFreeBsdState = Arc<Mutex<FreeBsdState>>;

pub fn new_shared() -> SharedFreeBsdState {
    Arc::new(Mutex::new(FreeBsdState::default()))
}

/// Check whether the required FreeBSD assets are present on disk.
pub fn assets_ready(guest_img_dir: &str, freebsd_ver: &str) -> bool {
    let edk2_dst  = format!("{}/edk2-aarch64-code.fd", guest_img_dir);
    let fbsd_img  = format!("{}/freebsd-{}-aarch64.img", guest_img_dir, freebsd_ver);
    std::path::Path::new(&edk2_dst).exists()
        && std::path::Path::new(&fbsd_img).exists()
}
