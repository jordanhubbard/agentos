//! Ubuntu Linux VM asset download and launch state.
//!
//! Tracks the phase of any in-progress Ubuntu VM preparation so that
//! the bridge API can report status.

use serde::Serialize;
use std::sync::{Arc, Mutex};

use crate::freebsd::VmExtraDevice;

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum LinuxVmPhase {
    Idle,
    Preparing,
    Running,
    Error,
}

#[derive(Debug, Clone, Serialize)]
pub struct LinuxVmState {
    pub phase:    LinuxVmPhase,
    pub step:     String,
    pub progress: u8,   // 0–100
    pub error:    Option<String>,
    pub qemu_pid: Option<u32>,
    /// Extra devices beyond the boot disk, seed disk, and primary NIC.
    pub devices:  Vec<VmExtraDevice>,
    /// Path to the QEMU QMP socket (set while QEMU is running).
    pub qmp_sock: Option<String>,
}

impl Default for LinuxVmState {
    fn default() -> Self {
        LinuxVmState {
            phase:    LinuxVmPhase::Idle,
            step:     String::new(),
            progress: 0,
            error:    None,
            qemu_pid: None,
            devices:  Vec::new(),
            qmp_sock: None,
        }
    }
}

pub type SharedLinuxVmState = Arc<Mutex<LinuxVmState>>;

pub fn new_shared() -> SharedLinuxVmState {
    Arc::new(Mutex::new(LinuxVmState::default()))
}

/// Check whether the required Ubuntu VM assets are on disk.
pub fn ubuntu_assets_ready(guest_img_dir: &str) -> bool {
    let ubuntu_img = format!("{}/ubuntu-24.04-aarch64.img", guest_img_dir);
    let seed_img   = format!("{}/linux-seed.img", guest_img_dir);
    std::path::Path::new(&ubuntu_img).exists()
        && std::path::Path::new(&seed_img).exists()
}

/// Create a cloud-init NoCloud seed disk at `seed_path`.
///
/// Writes `meta-data` and `user-data` into a temp directory then calls
/// `hdiutil makehybrid` (macOS) to produce an ISO 9660 + Joliet image
/// labelled "CIDATA". Cloud-init on Ubuntu recognises this label and
/// reads both files from it.
pub fn create_seed_disk(seed_path: &str) -> std::io::Result<()> {
    let tmp_dir = format!("/tmp/cidata-{}", std::process::id());
    std::fs::create_dir_all(&tmp_dir)?;

    std::fs::write(
        format!("{}/meta-data", tmp_dir),
        "instance-id: agentos-linux\nlocal-hostname: agentos-linux\n",
    )?;
    std::fs::write(
        format!("{}/user-data", tmp_dir),
        concat!(
            "#cloud-config\n",
            "password: agentos\n",
            "chpasswd: {expire: False}\n",
            "ssh_pwauth: true\n",
            "disable_root: false\n",
        ),
    )?;

    // hdiutil makehybrid always appends ".iso" to the output path when
    // creating an ISO image, so pass a tmp path and rename on success.
    let tmp_iso = format!("{}.tmp_seed", seed_path);
    let tmp_iso_actual = format!("{}.tmp_seed.iso", seed_path);

    let status = std::process::Command::new("hdiutil")
        .args([
            "makehybrid",
            "-quiet",
            "-o", &tmp_iso,
            "-iso",
            "-joliet",
            "-iso-volume-name",    "CIDATA",
            "-joliet-volume-name", "CIDATA",
            &tmp_dir,
        ])
        .status()?;

    let _ = std::fs::remove_dir_all(&tmp_dir);

    if !status.success() {
        let _ = std::fs::remove_file(&tmp_iso_actual);
        return Err(std::io::Error::new(
            std::io::ErrorKind::Other,
            format!("hdiutil makehybrid exited with: {}", status),
        ));
    }

    std::fs::rename(&tmp_iso_actual, seed_path)?;

    Ok(())
}
