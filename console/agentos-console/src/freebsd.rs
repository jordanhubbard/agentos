//! FreeBSD asset download state.
//!
//! Tracks the phase of any in-progress FreeBSD asset preparation so that
//! the bridge API can report status.

use serde::Serialize;
use std::sync::{Arc, Mutex};

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum FreeBsdPhase {
    Idle,
    Preparing,
    Running,
    Error,
}

#[derive(Debug, Clone, Serialize)]
pub struct FreeBsdState {
    pub phase:    FreeBsdPhase,
    pub step:     String,
    pub progress: u8,   // 0–100
    pub error:    Option<String>,
    pub qemu_pid: Option<u32>,
}

impl Default for FreeBsdState {
    fn default() -> Self {
        FreeBsdState {
            phase:    FreeBsdPhase::Idle,
            step:     String::new(),
            progress: 0,
            error:    None,
            qemu_pid: None,
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
