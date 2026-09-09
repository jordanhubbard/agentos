use crate::{rfb, TestArgs};
use anyhow::Context;
use std::io::{Read, Seek, SeekFrom, Write};
use std::net::{SocketAddr, TcpStream};
use std::ops::{Deref, DerefMut};
use std::os::unix::net::UnixStream;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Stdio};
use std::time::{Duration, Instant};

const UBUNTU_DEFAULT_SSH_PORT: u16 = 12222;
const FREEBSD_DEFAULT_SSH_PORT: u16 = 12223;
const UBUNTU_DESKTOP_PORT: u16 = 15901;
const UBUNTU_VNC_GUEST_PORT: u16 = 5901;
const UBUNTU_DESKTOP_RFB_TIMEOUT: Duration = Duration::from_secs(600);
const UBUNTU_DESKTOP_RFB_IO_TIMEOUT: Duration = Duration::from_secs(180);
const SSH_AUTH_OPTIONS: &[&str] = &[
    "-o",
    "BatchMode=yes",
    "-o",
    "PreferredAuthentications=publickey",
    "-o",
    "PasswordAuthentication=no",
    "-o",
    "KbdInteractiveAuthentication=no",
    "-o",
    "IdentitiesOnly=yes",
    "-o",
    "ConnectTimeout=30",
    "-o",
    "ConnectionAttempts=1",
    "-o",
    "StrictHostKeyChecking=no",
    "-o",
    "UserKnownHostsFile=/dev/null",
    "-o",
    "LogLevel=ERROR",
];
const SSH_PROBE_LIVENESS_OPTIONS: &[&str] =
    &["-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=1"];
const SSH_SESSION_LIVENESS_OPTIONS: &[&str] = &[
    "-o",
    "ServerAliveInterval=30",
    "-o",
    "ServerAliveCountMax=20",
];
const CC_WIRE_SHMEM_SIZE: usize = 4096;
const CC_INPUT_TEXT: u32 = 0x05;
/*
 * A text event crosses CC -> VibeEngine -> VM manager before reaching a
 * dynamic guest.  The 24-byte event plus a 4-byte Vibe handle and a 4-byte VM
 * slot must all fit the 48-byte seL4 payload, leaving 16 text bytes per frame.
 */
const CC_INPUT_TEXT_CHUNK: usize = 16;
const CC_REQ_SIZE: usize = 4 + 12 + CC_WIRE_SHMEM_SIZE;
const CC_REPLY_SIZE: usize = 16 + CC_WIRE_SHMEM_SIZE;
const CC_IO_TIMEOUT: Duration = Duration::from_secs(5);
/*
 * A console drain crosses the host virtconsole, CC-PD, vibe_engine,
 * vm_manager, and a running VMM. Under single-vCPU TCG, a fault-heavy guest
 * can legitimately delay that round trip beyond three host minutes. Keep the
 * same QEMU server-side chardev connection for the whole frame: opening a new
 * Unix connection only queues it in the listen backlog and does not switch
 * QEMU away from the current frontend.
 */
const CC_FRAME_DEADLINE: Duration = Duration::from_secs(600);
const CC_INPUT_RETRY_DEADLINE: Duration = Duration::from_secs(120);
const CC_OK: u32 = 0;
const CC_ERR_RELAY_FAULT: u32 = 8;
#[cfg(test)]
const VMM_RELAY_PAYLOAD_BYTES: usize = 48;
const MSG_CC_LOG_STREAM: u32 = 0x2610;
const MSG_CC_CREATE_GUEST: u32 = 0x2611;
const MSG_CC_SEND_INPUT: u32 = 0x260d;
const MSG_CC_SUSPEND_GUEST: u32 = 0x2613;
const MSG_CC_RESUME_GUEST: u32 = 0x2614;
const MSG_CC_DESTROY_GUEST: u32 = 0x2615;
const CC_INPUT_KEY_DOWN: u32 = 0x01;
const CC_INPUT_RAW_BYTE_BASE: u32 = 0x100;
const GUEST_DESTROY_NORMAL: u32 = 0;
const VIBEOS_TYPE_LINUX: u8 = 0x01;
const VIBEOS_TYPE_FREEBSD: u8 = 0x02;
const VIBEOS_ARCH_AARCH64: u8 = 0x01;
const VIBEOS_DEV_SERIAL: u32 = 1 << 0;
const VIBEOS_DEV_NET: u32 = 1 << 1;
const VIBEOS_DEV_BLOCK: u32 = 1 << 2;
const TRACE_PD_LINUX_VMM: u32 = 41;
const TRACE_PD_FREEBSD_VMM: u32 = 42;

pub fn run(args: &TestArgs) -> anyhow::Result<()> {
    let repo_root = repo_root()?;
    let ubuntu_live = args.assert_ubuntu_live || args.assert_desktop || args.guest_os == "both";

    anyhow::ensure!(
        !args.keep_running || args.guest_os == "both" || args.assert_desktop,
        "--keep-running requires --guest-os both or --assert-desktop"
    );
    if args.assert_emulated_net
        || args.assert_emulated_blk
        || args.assert_emulated_console
        || args.assert_agentos_virtio
        || args.assert_ubuntu_live
        || args.assert_desktop
    {
        anyhow::ensure!(
            args.board == "qemu_virt_aarch64",
            "emulated VirtIO assertions require --board qemu_virt_aarch64"
        );
        anyhow::ensure!(
            args.guest_os != "none",
            "emulated VirtIO assertions need a real guest; GUEST_OS=none is a stub VMM"
        );
    }
    if args.assert_emulated_console
        || args.assert_agentos_virtio
        || args.assert_ubuntu_live
        || args.assert_desktop
    {
        anyhow::ensure!(
            args.guest_os == "ubuntu",
            "Ubuntu VirtIO assertions require --guest-os ubuntu"
        );
    }

    if !args.no_build {
        println!(
            "[xtask:test] Building BOARD={} GUEST_OS={}...",
            args.board, args.guest_os
        );
        if ubuntu_live {
            run_make(
                &[
                    "build",
                    &format!("BOARD={}", args.board),
                    &format!("GUEST_OS={}", args.guest_os),
                    "UBUNTU_BOOT_MODE=live",
                ],
                &repo_root,
            )
            .context("live Ubuntu build step failed")?;
        } else {
            run_make(
                &[
                    "build",
                    &format!("BOARD={}", args.board),
                    &format!("GUEST_OS={}", args.guest_os),
                ],
                &repo_root,
            )
            .context("build step failed")?;
        }
    }

    let tmp_dir = repo_root.join("build/tmp");
    std::fs::create_dir_all(&tmp_dir)
        .with_context(|| format!("failed to create {}", tmp_dir.display()))?;
    let log_file = tempfile::Builder::new()
        .prefix("agentos-qemu-")
        .suffix(".log")
        .tempfile_in(&tmp_dir)
        .context("failed to create build/tmp QEMU log file")?;
    let (_, log_path) = log_file
        .keep()
        .context("failed to persist build/tmp QEMU log file")?;
    let ssh_key = if args.guest_os == "both" || args.assert_desktop {
        Some(generate_ssh_test_key(&repo_root, args.keep_running)?)
    } else {
        None
    };

    let needs_ssh_probe = !matches!(args.guest_os.as_str(), "ubuntu" | "freebsd");
    let needs_host_net_stimulus =
        args.guest_os == "ubuntu" && (args.assert_agentos_virtio || args.assert_ubuntu_live);
    let ssh_port = if needs_ssh_probe || needs_host_net_stimulus || args.assert_desktop {
        effective_ssh_port(args)
    } else {
        0
    };
    println!("[xtask:test] Launching QEMU for board={}...", args.board);
    let cc_sock = log_path.with_extension("cc_pd.sock");
    let mut qemu = ChildGuard::new(spawn_qemu_with_guest(
        &args.board,
        &repo_root,
        &log_path,
        &cc_sock,
        &args.guest_os,
        ssh_port,
        ubuntu_live,
        args.assert_desktop && !args.keep_running,
    )?);
    if needs_host_net_stimulus {
        wait_for_all_markers(
            &log_path,
            &["emulated virtio-net: guest DRIVER_OK"],
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        )
        .context("guest net driver did not become ready for host RX stimulus")?;
        /* The SYN is sufficient evidence and RX stimulus. Holding this
         * pre-sshd connection can consume the first daemon accept slot. */
        drop(connect_host_net_stimulus(ssh_port, &mut qemu));
    }

    let mut result = if args.assert_emulated_net {
        println!(
            "[xtask:test] Waiting for emulated virtio-net guest proof in {}...",
            log_path.display()
        );
        wait_for_emulated_net(&log_path, Duration::from_secs(args.timeout_secs), &mut qemu)
    } else if args.assert_emulated_blk {
        println!(
            "[xtask:test] Waiting for emulated virtio-blk guest proof in {}...",
            log_path.display()
        );
        wait_for_emulated_blk(&log_path, Duration::from_secs(args.timeout_secs), &mut qemu)
    } else {
        match args.guest_os.as_str() {
            "ubuntu" => {
                println!(
                    "[xtask:test] Waiting for Ubuntu login prompt via CC-PD API ({})...",
                    cc_sock.display()
                );
                wait_for_guest_console_login_via_cc(
                    &cc_sock,
                    0,
                    if ubuntu_live { "ubuntu-live" } else { "ubuntu" },
                    Duration::from_secs(args.timeout_secs),
                    &mut qemu,
                )
            }
            "freebsd" => {
                println!(
                    "[xtask:test] Waiting for FreeBSD login prompt via CC-PD API ({})...",
                    cc_sock.display()
                );
                wait_for_guest_console_login_via_cc(
                    &cc_sock,
                    0,
                    "freebsd",
                    Duration::from_secs(args.timeout_secs),
                    &mut qemu,
                )
            }
            "both" => {
                println!(
                    "[xtask:test] Creating FreeBSD and Linux through CC-PD/vm_manager ({})...",
                    cc_sock.display()
                );
                wait_for_dual_guest_consoles_via_cc(
                    &cc_sock,
                    Duration::from_secs(args.timeout_secs),
                    &mut qemu,
                    ssh_key.as_ref().context("dual SSH key was not generated")?,
                    args.keep_running,
                )
            }
            _ => {
                /* Success markers: any match is a pass.
                 * "agentOS boot complete" = root task + all PDs launched.
                 * "[rt] boot complete"    = x86 root-task smoke boot; service PD
                 *                           runtime health is tracked separately.
                 * "buildroot login:"      = Linux guest reached login prompt (buildroot). */
                if args.board == "x86_64_generic" {
                    wait_for_x86_reduced_smoke(&log_path, Duration::from_secs(args.timeout_secs))
                } else {
                    wait_for_markers(
                        &log_path,
                        &["agentOS boot complete", "buildroot login:"],
                        Duration::from_secs(args.timeout_secs),
                    )
                }
            }
        }
    };

    /*
     * Start the authenticated desktop path before checking host-backed network
     * markers.  Its SSH connection is the RX stimulus.  Connecting to the
     * forwarded port before sshd exists leaves a stale user-net flow that can
     * accept later host sockets without ever completing an SSH banner.
     */
    let mut desktop_evidence = None;
    let mut desktop_tunnel = None;
    if result.is_ok() && args.assert_desktop {
        let key = ssh_key
            .as_ref()
            .context("desktop SSH key was not generated")?;
        match prove_ubuntu_desktop(
            &cc_sock,
            key,
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        ) {
            Ok((evidence, tunnel)) => {
                desktop_evidence = Some(evidence);
                desktop_tunnel = Some(tunnel);
            }
            Err(error) => result = Err(error),
        }
    }

    if result.is_ok()
        && (args.assert_emulated_console
            || args.assert_agentos_virtio
            || args.assert_ubuntu_live
            || args.assert_desktop)
    {
        let mut required = vec![
            "emulated virtio-console: guest probed",
            "emulated virtio-console: guest DRIVER_OK",
            "emulated virtio-console: pumped ",
            "emulated virtio-console: pumped input serial_virt->guest",
        ];
        if args.assert_agentos_virtio || args.assert_ubuntu_live || args.assert_desktop {
            required.extend_from_slice(&[
                "emulated virtio-net: guest probed",
                "emulated virtio-net: guest DRIVER_OK",
                "emulated virtio-net: backend TX accepted by net_pd",
                "emulated virtio-net: backend RX delivered from net_pd",
                "emulated virtio-net: pumped ",
                "via host-backed net_pd",
                "[net_pd] HOST_READY: virtio-net bus.16",
                "[net_pd] HOST_TX: QEMU bus.16 completion observed",
                "emulated virtio-blk: guest probed",
                "emulated virtio-blk: guest DRIVER_OK",
                "emulated virtio-blk: pumped",
                "emulated virtio-blk: agentOS host media 0 ready",
                "emulated virtio-blk: host-media read",
            ]);
        }
        let console =
            wait_for_all_markers(&log_path, &required, Duration::from_secs(10), &mut qemu);
        let network_proof_required =
            args.assert_agentos_virtio || args.assert_ubuntu_live || args.assert_desktop;
        let no_loopback = !network_proof_required
            || std::fs::read_to_string(&log_path)
                .map(|log| !log.contains("frame(s) TX->RX"))
                .unwrap_or(false);
        result = match (result, console) {
            (Ok(_), Ok(_)) if !no_loopback => {
                anyhow::bail!("Ubuntu network proof used the forbidden VMM-local TX->RX loopback")
            }
            (Ok(login), Ok(_)) if args.assert_ubuntu_live || args.assert_desktop => Ok(format!(
                "{login}; full Ubuntu Casper userspace uses agentOS virtio net + blk + console"
            )),
            (Ok(login), Ok(_)) if args.assert_agentos_virtio => Ok(format!(
                "{login}; agentOS virtio net + blk + console probed, DRIVER_OK, and pumped real I/O"
            )),
            (Ok(login), Ok(_)) => Ok(format!(
                "{login}; emulated virtio-console probed + DRIVER_OK + bidirectional I/O"
            )),
            (_, Err(err)) => {
                Err(err
                    .context("Ubuntu login succeeded but host-backed virtio proof was incomplete"))
            }
            (Err(err), _) => Err(err),
        };
    }

    if result.is_ok() {
        if let Some(evidence) = desktop_evidence {
            result = Ok(format!(
                "{}; Ubuntu desktop RFB {}x{} bytes={} fnv1a64={:016x} name={:?}",
                result.as_deref().unwrap_or("Ubuntu live guest ready"),
                evidence.width,
                evidence.height,
                evidence.bytes_received,
                evidence.fnv1a64,
                evidence.desktop_name,
            ));
        }
    }

    if args.keep_running && result.is_ok() {
        let key = ssh_key
            .as_ref()
            .context("persistent SSH key was not generated")?;
        result = if args.assert_desktop {
            wait_for_manual_desktop(key, &mut qemu)
                .map(|()| String::from("manual Ubuntu desktop session completed"))
        } else {
            wait_for_manual_dual_ssh(key, &mut qemu)
                .map(|()| String::from("manual dual SSH session completed"))
        };
    }

    if let Some(mut tunnel) = desktop_tunnel {
        let _ = tunnel.kill();
        let _ = tunnel.wait();
    }
    let _ = qemu.kill();
    let _ = qemu.wait();

    // Print captured serial output
    println!("\n=== Serial output ===");
    if let Ok(mut f) = std::fs::File::open(&log_path) {
        let mut buf = String::new();
        let _ = f.read_to_string(&mut buf);
        print!("{}", buf);
    }
    println!("=====================\n");

    match result {
        Ok(marker) => {
            println!("PASS [board={}]: found marker \"{}\"", args.board, marker);
            Ok(())
        }
        Err(e) => {
            println!("FAIL [board={}]: {:#}", args.board, e);
            anyhow::bail!("test failed for board {}: {:#}", args.board, e);
        }
    }
}

fn wait_for_manual_dual_ssh(ssh_key: &SshTestKey, qemu: &mut Child) -> anyhow::Result<()> {
    ensure_qemu_running(qemu, "entering manual dual SSH mode")?;
    println!("\nDual guests are running with authenticated SSH:");
    for command in manual_ssh_commands(&ssh_key.private_key) {
        println!("  {command}");
    }
    println!("Press Enter here to stop both guests and QEMU.");

    let mut line = String::new();
    let bytes = std::io::stdin()
        .read_line(&mut line)
        .context("failed to wait for manual SSH shutdown input")?;
    anyhow::ensure!(bytes != 0, "manual SSH mode requires an interactive stdin");
    ensure_qemu_running(qemu, "leaving manual dual SSH mode")
}

fn wait_for_manual_desktop(ssh_key: &SshTestKey, qemu: &mut Child) -> anyhow::Result<()> {
    ensure_qemu_running(qemu, "entering manual Ubuntu desktop mode")?;
    println!("\nUbuntu is running a tunnel-confined VNC desktop:");
    println!("  vncviewer 127.0.0.1:{}", UBUNTU_DESKTOP_PORT);
    println!(
        "  ssh -i '{}' -p {} -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null ubuntu@127.0.0.1",
        ssh_key.private_key.display(),
        UBUNTU_DEFAULT_SSH_PORT
    );
    println!("Press Enter here to stop the guest, SSH tunnel, and QEMU.");

    let mut line = String::new();
    let bytes = std::io::stdin()
        .read_line(&mut line)
        .context("failed to wait for manual desktop shutdown input")?;
    anyhow::ensure!(
        bytes != 0,
        "manual desktop mode requires an interactive stdin"
    );
    ensure_qemu_running(qemu, "leaving manual Ubuntu desktop mode")
}

fn manual_ssh_commands(private_key: &Path) -> [String; 2] {
    [
        format!(
            "ssh -i '{}' -p {} -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null ubuntu@127.0.0.1",
            private_key.display(),
            UBUNTU_DEFAULT_SSH_PORT
        ),
        format!(
            "ssh -i '{}' -p {} -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@127.0.0.1",
            private_key.display(),
            FREEBSD_DEFAULT_SSH_PORT
        ),
    ]
}

fn effective_ssh_port(args: &TestArgs) -> u16 {
    if (args.guest_os == "ubuntu" || args.guest_os == "both") && args.ssh_port == 0 {
        UBUNTU_DEFAULT_SSH_PORT
    } else if args.guest_os == "freebsd" && args.ssh_port == 0 {
        FREEBSD_DEFAULT_SSH_PORT
    } else {
        args.ssh_port
    }
}

struct SshTestKey {
    _temporary_dir: Option<tempfile::TempDir>,
    private_key: PathBuf,
    public_key: String,
}

struct ChildGuard {
    child: Child,
}

impl ChildGuard {
    fn new(child: Child) -> Self {
        Self { child }
    }
}

impl Deref for ChildGuard {
    type Target = Child;

    fn deref(&self) -> &Self::Target {
        &self.child
    }
}

impl DerefMut for ChildGuard {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.child
    }
}

impl Drop for ChildGuard {
    fn drop(&mut self) {
        if matches!(self.child.try_wait(), Ok(None)) {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }
}

fn generate_ssh_test_key(repo_root: &Path, persistent: bool) -> anyhow::Result<SshTestKey> {
    let tmp_dir = repo_root.join("build/tmp");
    std::fs::create_dir_all(&tmp_dir)
        .with_context(|| format!("failed to create {}", tmp_dir.display()))?;
    let (temporary_dir, key_dir) = if persistent {
        let key_dir = tmp_dir.join("dual-ssh");
        std::fs::create_dir_all(&key_dir)
            .with_context(|| format!("failed to create {}", key_dir.display()))?;
        (None, key_dir)
    } else {
        let dir = tempfile::Builder::new()
            .prefix("agentos-dual-ssh-")
            .tempdir_in(&tmp_dir)
            .context("failed to create dual SSH key directory")?;
        let key_dir = dir.path().to_path_buf();
        (Some(dir), key_dir)
    };
    let private_key = key_dir.join("id_ed25519");
    let public_key_path = private_key.with_extension("pub");
    for path in [&private_key, &public_key_path] {
        if path.exists() {
            std::fs::remove_file(path)
                .with_context(|| format!("failed to replace {}", path.display()))?;
        }
    }
    let status = std::process::Command::new("ssh-keygen")
        .args(["-q", "-t", "ed25519", "-N", "", "-f"])
        .arg(&private_key)
        .status()
        .context("failed to run ssh-keygen for dual SSH proof")?;
    anyhow::ensure!(status.success(), "ssh-keygen failed with {status}");
    let public_key = std::fs::read_to_string(public_key_path)
        .context("failed to read generated dual SSH public key")?
        .trim()
        .to_string();
    Ok(SshTestKey {
        _temporary_dir: temporary_dir,
        private_key,
        public_key,
    })
}

pub fn run_make(args: &[&str], cwd: &Path) -> anyhow::Result<()> {
    let status = std::process::Command::new("make")
        .args(args)
        .current_dir(cwd)
        .status()?;
    anyhow::ensure!(status.success(), "make {} failed", args.join(" "));
    Ok(())
}

fn repo_root() -> anyhow::Result<std::path::PathBuf> {
    // Walk up from the xtask binary's manifest dir or use CARGO_MANIFEST_DIR
    // At runtime, resolve relative to the current working directory's git root.
    let output = std::process::Command::new("git")
        .args(["rev-parse", "--show-toplevel"])
        .output()
        .context("failed to run git rev-parse")?;
    anyhow::ensure!(output.status.success(), "not in a git repository");
    let root = String::from_utf8(output.stdout)
        .context("git output is not utf-8")?
        .trim()
        .to_string();
    Ok(std::path::PathBuf::from(root))
}

pub(crate) fn sel4_sdk_path() -> anyhow::Result<PathBuf> {
    if let Some(path) = std::env::var_os("SEL4_SDK") {
        return Ok(PathBuf::from(path));
    }

    let home = std::env::var_os("HOME").context(
        "SEL4_SDK is unset and HOME is unavailable; set SEL4_SDK to the external Microkit SDK",
    )?;
    Ok(PathBuf::from(home).join(".cache/agentos/microkit-sdk-2.1.0"))
}

pub fn spawn_qemu_with_guest(
    board: &str,
    repo_root: &Path,
    log_path: &Path,
    cc_sock: &Path,
    guest_os: &str,
    ssh_port: u16,
    ubuntu_live: bool,
    capture_net: bool,
) -> anyhow::Result<std::process::Child> {
    let log_file = std::fs::File::create(log_path).context("failed to create QEMU log file")?;
    let netdev = qemu_netdev_arg(ssh_port, guest_os)?;

    let build_image = repo_root.join("build").join(board).join("agentos.img");

    let mut cmd = match board {
        "qemu_virt_aarch64" => {
            let build_dir = repo_root.join("build").join(board);
            let loader = build_dir.join("loader.elf");
            let _ = std::fs::remove_file(&cc_sock);
            let machine = if guest_os == "freebsd" || guest_os == "both" {
                "virt,virtualization=on,highmem=off,secure=off,acpi=off"
            } else {
                "virt,virtualization=on,highmem=off,secure=off"
            };
            let memory = if guest_os == "both" || ubuntu_live {
                "3G"
            } else {
                "2G"
            };
            let sel4_profile =
                std::env::var("SEL4_PROFILE").unwrap_or_else(|_| String::from("release"));
            let smp = if sel4_profile.starts_with("smp-") || sel4_profile == "smp" {
                "4"
            } else {
                "1"
            };
            let mut c = std::process::Command::new("qemu-system-aarch64");
            c.arg("-machine")
                .arg(machine)
                .arg("-cpu")
                .arg("cortex-a57")
                .arg("-m")
                .arg(memory)
                .arg("-smp")
                .arg(smp)
                .arg("-display")
                .arg("none")
                .arg("-monitor")
                .arg("none")
                .arg("-serial")
                .arg(format!("file:{}", log_path.display()))
                .arg("-global")
                .arg("virtio-mmio.force-legacy=off")
                .arg("-chardev")
                .arg(format!(
                    "socket,id=cc_pd_char,path={},server=on,wait=off",
                    cc_sock.display()
                ))
                .arg("-device")
                .arg("virtio-serial-device,bus=virtio-mmio-bus.2,id=vser0")
                .arg("-device")
                .arg("virtconsole,bus=vser0.0,chardev=cc_pd_char,name=cc.0")
                .arg("-device")
                .arg(format!("loader,file={},cpu-num=0", loader.display()))
                .arg("-device")
                .arg(format!(
                    "loader,file={},addr=0x48000000",
                    build_image.display()
                ));
            /*
             * Page-isolated host transport owned by net_pd. Every guest sees
             * only its separately emulated device at IPA 0x0a010000.
             */
            c.args([
                "-device",
                "virtio-net-device,netdev=net0,bus=virtio-mmio-bus.16,mac=02:00:00:00:00:01,ctrl_vq=off,mq=off",
                "-netdev",
                &netdev,
            ]);
            if guest_os == "ubuntu" || guest_os == "both" {
                /*
                 * Ubuntu media is host hardware on bus.8, owned only by the
                 * agentOS virtio_blk PD. The guest DTB advertises only the
                 * emulated device at 0x0a020000 in single and dual mode.
                 */
                let ubuntu_img = ubuntu_disk_image(repo_root);
                if ubuntu_img.exists() {
                    println!(
                        "[xtask:test] agentOS host block media: {}",
                        ubuntu_img.display()
                    );
                    c.args([
                        "-device",
                        "virtio-blk-device,drive=agentos_hd,bus=virtio-mmio-bus.8",
                        "-drive",
                        &format!(
                            "file={},format=raw,id=agentos_hd,if=none,readonly=on,file.locking=off",
                            ubuntu_img.to_str().unwrap()
                        ),
                    ]);
                }
            }
            if guest_os == "freebsd" || guest_os == "both" {
                let freebsd_img = freebsd_disk_image(repo_root);
                if freebsd_img.exists() {
                    println!(
                        "[xtask:test] agentOS FreeBSD host block media: {}",
                        freebsd_img.display()
                    );
                    /*
                     * bus.31 is host hardware owned only by the canonical
                     * block-service PD. FreeBSD sees the emulated endpoint at
                     * 0x0a020000 and never receives this MMIO page.
                     */
                    c.args([
                        "-device",
                        "virtio-blk-device,drive=freebsd_hd,bus=virtio-mmio-bus.31",
                        "-drive",
                        &format!(
                            "file={},format=raw,id=freebsd_hd,if=none,readonly=on,file.locking=off",
                            freebsd_img.to_str().unwrap()
                        ),
                    ]);
                }
            }
            c
        }
        "qemu_virt_riscv64" => {
            let bios = find_opensbi_bios();
            let mut c = std::process::Command::new("qemu-system-riscv64");
            c.args([
                "-machine",
                "virt",
                "-cpu",
                "rv64",
                "-m",
                "2G",
                "-nographic",
                "-bios",
                &bios,
                "-kernel",
                build_image
                    .to_str()
                    .unwrap_or("build/qemu_virt_riscv64/agentos.img"),
                /* virtio-net (slot 0 → 0x10001000, IRQ 1) with SSH port forward */
                "-device",
                "virtio-net-device,netdev=net0",
                "-netdev",
                &netdev,
            ]);
            /* virtio-blk (slot 1 → 0x10002000, IRQ 2) — only if disk image exists */
            let disk = repo_root.join("build/qemu_virt_riscv64/disk.img");
            if disk.exists() {
                c.args([
                    "-device",
                    "virtio-blk-device,drive=hd0",
                    "-drive",
                    &format!(
                        "file={},format=raw,id=hd0,if=none",
                        disk.to_str().unwrap_or("build/qemu_virt_riscv64/disk.img")
                    ),
                ]);
            }
            c
        }
        "x86_64_generic" => {
            let kernel = sel4_sdk_path()?.join("board/x86_64_generic/release/elf/sel4_32.elf");
            let root_task = repo_root.join("build/x86_64_generic/root_task.elf");
            let mut c = std::process::Command::new("qemu-system-x86_64");
            c.arg("-machine")
                .arg("q35")
                .arg("-cpu")
                .arg("max")
                .arg("-m")
                .arg("2G")
                .arg("-display")
                .arg("none")
                .arg("-monitor")
                .arg("none")
                .arg("-serial")
                .arg("stdio")
                .arg("-kernel")
                .arg(kernel)
                .arg("-initrd")
                .arg(root_task)
                .arg("-netdev")
                .arg(netdev)
                .arg("-device")
                .arg("e1000,netdev=net0");
            c
        }
        other => {
            anyhow::bail!(
                "unknown board: {} — add QEMU invocation to cmd_test.rs",
                other
            );
        }
    };

    if capture_net {
        let capture_path = log_path.with_extension("net.pcap");
        println!(
            "[xtask:test] Capturing host-backed guest packets in {}",
            capture_path.display()
        );
        cmd.arg("-object").arg(format!(
            "filter-dump,id=agentos_net_capture,netdev=net0,file={}",
            capture_path.display()
        ));
    }

    let child = if board == "qemu_virt_aarch64" {
        let stderr_path = log_path.with_extension("qemu.stderr");
        let stderr_file = std::fs::File::create(&stderr_path)
            .with_context(|| format!("failed to create {}", stderr_path.display()))?;
        println!("[xtask:test] QEMU stderr: {}", stderr_path.display());
        cmd.stdout(std::process::Stdio::null())
            .stderr(stderr_file)
            .process_group(0)
            .spawn()
            .context("failed to spawn QEMU")?
    } else {
        cmd.stdout(log_file.try_clone()?)
            .stderr(log_file)
            .process_group(0)
            .spawn()
            .context("failed to spawn QEMU")?
    };
    println!("[xtask:test] QEMU pid={}", child.id());
    Ok(child)
}

fn ubuntu_disk_image(repo_root: &Path) -> std::path::PathBuf {
    repo_root.join("build/guest-images/ubuntu-26.04-aarch64.iso")
}

fn freebsd_disk_image(repo_root: &Path) -> std::path::PathBuf {
    if let Ok(path) = std::env::var("AGENTOS_FREEBSD_IMAGE") {
        let override_path = std::path::PathBuf::from(path);
        if override_path.exists() {
            return override_path;
        }
    }

    repo_root.join("build/guest-images/freebsd-15.0-aarch64.iso")
}

fn qemu_netdev_arg(ssh_port: u16, guest_os: &str) -> anyhow::Result<String> {
    if ssh_port == 0 {
        return Ok("user,id=net0".to_string());
    }
    ensure_host_port_available(ssh_port)?;
    if guest_os == "both" {
        anyhow::ensure!(
            ssh_port != FREEBSD_DEFAULT_SSH_PORT,
            "dual guest Ubuntu and FreeBSD SSH ports must differ"
        );
        ensure_host_port_available(FREEBSD_DEFAULT_SSH_PORT)?;
        return Ok(dual_qemu_netdev_arg(ssh_port));
    }
    Ok(format!(
        "user,id=net0,hostfwd=tcp:127.0.0.1:{}-:22",
        ssh_port
    ))
}

fn dual_qemu_netdev_arg(ubuntu_port: u16) -> String {
    format!(
        "user,id=net0,hostfwd=tcp:127.0.0.1:{ubuntu_port}-10.0.2.15:22,hostfwd=tcp:127.0.0.1:{FREEBSD_DEFAULT_SSH_PORT}-10.0.2.16:22"
    )
}

fn ensure_host_port_available(port: u16) -> anyhow::Result<()> {
    let listener = std::net::TcpListener::bind(("127.0.0.1", port)).with_context(|| {
        format!(
            "host TCP port {} is already in use; pass --ssh-port 0 to disable SSH forwarding or choose another port",
            port
        )
    })?;
    drop(listener);
    Ok(())
}

/// Locate the OpenSBI RISCV64 firmware binary, searching common locations.
fn find_opensbi_bios() -> String {
    let candidates = [
        // macOS Homebrew (both Intel and Apple Silicon)
        "/opt/homebrew/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin",
        "/usr/local/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin",
        // Linux system package
        "/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin",
        // Debian/Ubuntu alternate path
        "/usr/lib/riscv64-linux-gnu/opensbi/generic/fw_dynamic.bin",
    ];
    for path in candidates {
        if std::path::Path::new(path).exists() {
            return path.to_string();
        }
    }
    // Also check via `brew --prefix` at runtime for non-standard Homebrew roots
    if let Ok(output) = std::process::Command::new("brew")
        .args(["--prefix"])
        .output()
    {
        if let Ok(prefix) = std::str::from_utf8(&output.stdout) {
            let p = format!(
                "{}/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin",
                prefix.trim()
            );
            if std::path::Path::new(&p).exists() {
                return p;
            }
        }
    }
    // Fall back to the Linux path and let QEMU report a clear error
    "/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin".to_string()
}

/// Poll the log file until one of `markers` appears or `timeout` elapses.
/// Returns the matched marker string on success.
pub fn wait_for_markers(
    log_path: &Path,
    markers: &[&str],
    timeout: Duration,
) -> anyhow::Result<String> {
    let start = Instant::now();
    let mut file = std::fs::File::open(log_path).context("failed to open log file")?;
    let mut offset: u64 = 0;
    let mut accumulated = String::new();

    loop {
        if start.elapsed() >= timeout {
            anyhow::bail!("timeout after {}s", timeout.as_secs());
        }

        file.seek(SeekFrom::Start(offset))?;
        let mut raw = Vec::new();
        let bytes_read = file.read_to_end(&mut raw)?;
        if bytes_read > 0 {
            offset += bytes_read as u64;
            // QEMU may emit non-UTF-8 bytes (e.g. from OpenSBI/seL4 early boot);
            // replace invalid sequences rather than failing.
            accumulated.push_str(&String::from_utf8_lossy(&raw));

            for &marker in markers {
                if accumulated.contains(marker) {
                    return Ok(marker.to_string());
                }
            }
        }

        std::thread::sleep(Duration::from_millis(200));
    }
}

fn wait_for_all_markers(
    log_path: &Path,
    markers: &[&str],
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    let start = Instant::now();
    let mut file = std::fs::File::open(log_path).context("failed to open log file")?;
    let mut offset: u64 = 0;
    let mut accumulated = String::new();

    loop {
        ensure_qemu_running(qemu, "waiting for required runtime evidence")?;
        if start.elapsed() >= timeout {
            let missing: Vec<&str> = markers
                .iter()
                .copied()
                .filter(|marker| !accumulated.contains(marker))
                .collect();
            anyhow::bail!(
                "timeout after {}s waiting for all markers; missing {:?}",
                timeout.as_secs(),
                missing
            );
        }

        file.seek(SeekFrom::Start(offset))?;
        let mut raw = Vec::new();
        let bytes_read = file.read_to_end(&mut raw)?;
        if bytes_read > 0 {
            offset += bytes_read as u64;
            accumulated.push_str(&String::from_utf8_lossy(&raw));
            if markers.iter().all(|marker| accumulated.contains(marker)) {
                return Ok(markers.join(" + "));
            }
        }

        std::thread::sleep(Duration::from_millis(200));
    }
}

const EMU_NET_REQUIRED: &[&str] = &[
    "emulated virtio-net: guest probed",
    "emulated virtio-net: guest DRIVER_OK",
    "emulated virtio-net: pumped",
];

const EMU_NET_GUEST_ANY: &[&str] = &[
    "0a010000.virtio_mmio",
    "a010000.virtio_mmio",
    "02:00:00:00:00:01",
    "Sending DHCP requests",
];

const EMU_BLK_REQUIRED: &[&str] = &[
    "emulated virtio-blk: guest probed",
    "emulated virtio-blk: guest DRIVER_OK",
    "emulated virtio-blk: pumped",
    "emulated virtio-blk: drain=",
];

fn wait_for_emulated_blk(
    log_path: &Path,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    let start = Instant::now();
    let mut file = std::fs::File::open(log_path).context("failed to open log file")?;
    let mut offset: u64 = 0;
    let mut accumulated = String::new();

    loop {
        ensure_qemu_running(qemu, "waiting for emulated virtio-blk proof")?;
        if start.elapsed() >= timeout {
            let missing: Vec<&str> = EMU_BLK_REQUIRED
                .iter()
                .copied()
                .filter(|m| !accumulated.contains(m))
                .collect();
            anyhow::bail!(
                "emulated virtio-blk proof timeout after {}s; missing VMM markers {:?}",
                timeout.as_secs(),
                missing,
            );
        }

        file.seek(SeekFrom::Start(offset))?;
        let mut raw = Vec::new();
        let bytes_read = file.read_to_end(&mut raw)?;
        if bytes_read > 0 {
            offset += bytes_read as u64;
            accumulated.push_str(&String::from_utf8_lossy(&raw));

            let vmm_ok = EMU_BLK_REQUIRED.iter().all(|m| accumulated.contains(m));
            if vmm_ok {
                return Ok(
                    "emulated virtio-blk: guest configured queue + DRIVER_OK + completed request"
                        .to_string(),
                );
            }
        }

        std::thread::sleep(Duration::from_millis(200));
    }
}

fn wait_for_emulated_net(
    log_path: &Path,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    let start = Instant::now();
    let mut file = std::fs::File::open(log_path).context("failed to open log file")?;
    let mut offset: u64 = 0;
    let mut accumulated = String::new();

    loop {
        ensure_qemu_running(qemu, "waiting for emulated virtio-net proof")?;
        if start.elapsed() >= timeout {
            let missing: Vec<&str> = EMU_NET_REQUIRED
                .iter()
                .copied()
                .filter(|m| !accumulated.contains(m))
                .collect();
            let guest_ok = EMU_NET_GUEST_ANY.iter().any(|m| accumulated.contains(m));
            anyhow::bail!(
                "emulated virtio-net proof timeout after {}s; missing VMM markers {:?}; guest IPA/MAC observed={}",
                timeout.as_secs(),
                missing,
                guest_ok
            );
        }

        file.seek(SeekFrom::Start(offset))?;
        let mut raw = Vec::new();
        let bytes_read = file.read_to_end(&mut raw)?;
        if bytes_read > 0 {
            offset += bytes_read as u64;
            accumulated.push_str(&String::from_utf8_lossy(&raw));

            let vmm_ok = EMU_NET_REQUIRED.iter().all(|m| accumulated.contains(m));
            let guest_ok = EMU_NET_GUEST_ANY.iter().any(|m| accumulated.contains(m));
            if vmm_ok && guest_ok {
                return Ok(
                    "emulated virtio-net: guest probed + DRIVER_OK + pumped + guest IPA/MAC"
                        .to_string(),
                );
            }
        }

        std::thread::sleep(Duration::from_millis(200));
    }
}

fn wait_for_x86_reduced_smoke(log_path: &Path, timeout: Duration) -> anyhow::Result<String> {
    let marker = wait_for_markers(log_path, &["[rt] boot complete"], timeout)?;
    std::thread::sleep(Duration::from_secs(2));

    let output = std::fs::read_to_string(log_path).unwrap_or_default();
    anyhow::ensure!(
        !output.contains("[rt] FAULT"),
        "x86 reduced smoke emitted root-task fault endpoint reports"
    );
    Ok(format!(
        "{marker} (x86 reduced smoke, no fault endpoint reports)"
    ))
}

pub struct CcReply {
    pub mr: [u32; 4],
    pub shmem: Vec<u8>,
}

struct CcClient {
    stream: Option<UnixStream>,
    socket_path: PathBuf,
}

impl CcClient {
    fn connect(cc_sock: &Path) -> anyhow::Result<Self> {
        let stream = Self::connect_stream(cc_sock)?;
        Ok(Self {
            stream: Some(stream),
            socket_path: cc_sock.to_path_buf(),
        })
    }

    fn connect_stream(cc_sock: &Path) -> anyhow::Result<UnixStream> {
        let stream = UnixStream::connect(cc_sock)
            .with_context(|| format!("failed to connect to {}", cc_sock.display()))?;
        stream
            .set_read_timeout(Some(CC_IO_TIMEOUT))
            .context("failed to set CC socket read timeout")?;
        stream
            .set_write_timeout(Some(CC_IO_TIMEOUT))
            .context("failed to set CC socket write timeout")?;
        Ok(stream)
    }

    fn is_closed(&self) -> bool {
        self.stream.is_none()
    }

    fn call(
        &mut self,
        opcode: u32,
        mr1: u32,
        mr2: u32,
        mr3: u32,
        shmem_in: &[u8],
    ) -> anyhow::Result<CcReply> {
        let mut req = [0u8; CC_REQ_SIZE];
        wr32(&mut req, 0, opcode);
        wr32(&mut req, 4, mr1);
        wr32(&mut req, 8, mr2);
        wr32(&mut req, 12, mr3);
        let copy_len = shmem_in.len().min(CC_WIRE_SHMEM_SIZE);
        if copy_len > 0 {
            req[16..16 + copy_len].copy_from_slice(&shmem_in[..copy_len]);
        }

        let first_error = match self.call_frame(&req) {
            Ok(reply) => return Ok(reply),
            Err(error) => error,
        };
        self.stream.take();

        /*
         * A CC operation can complete before the socket-backed VirtIO reply
         * becomes writable.  Dropping the old frontend lets QEMU accept a new
         * connection; resending the identical frame is safe because CC-PD
         * records state-changing replies before resetting a stalled TX queue.
         */
        let stream = Self::connect_stream(&self.socket_path).with_context(|| {
            format!("CC reconnect after incomplete reply failed; first error: {first_error:#}")
        })?;
        self.stream = Some(stream);
        self.call_frame(&req).with_context(|| {
            format!("CC replay after incomplete reply failed; first error: {first_error:#}")
        })
    }

    fn call_frame(&mut self, req: &[u8; CC_REQ_SIZE]) -> anyhow::Result<CcReply> {
        let result: anyhow::Result<CcReply> = (|| {
            let stream = self.stream.as_mut().context("CC connection is closed")?;
            write_cc_frame(stream, req).context("failed to write CC request")?;

            let mut raw = [0u8; CC_REPLY_SIZE];
            read_cc_frame(stream, &mut raw)?;
            Ok(CcReply {
                mr: [rd32(&raw, 0), rd32(&raw, 4), rd32(&raw, 8), rd32(&raw, 12)],
                shmem: raw[16..].to_vec(),
            })
        })();
        if result.is_err() {
            self.stream.take();
        }
        result
    }
}

fn retryable_cc_io(err: &std::io::Error) -> bool {
    matches!(
        err.kind(),
        std::io::ErrorKind::Interrupted
            | std::io::ErrorKind::WouldBlock
            | std::io::ErrorKind::TimedOut
    )
}

fn write_cc_frame(stream: &mut UnixStream, frame: &[u8]) -> anyhow::Result<()> {
    let start = Instant::now();
    let mut written = 0usize;
    while written < frame.len() {
        match stream.write(&frame[written..]) {
            Ok(0) => anyhow::bail!("CC connection closed while writing frame"),
            Ok(count) => written += count,
            Err(err) if retryable_cc_io(&err) && start.elapsed() < CC_FRAME_DEADLINE => {}
            Err(err) if retryable_cc_io(&err) => {
                anyhow::bail!("timed out writing CC frame after {} bytes", written)
            }
            Err(err) => return Err(err).context("failed to write CC frame"),
        }
    }
    Ok(())
}

fn read_cc_frame(stream: &mut UnixStream, frame: &mut [u8]) -> anyhow::Result<()> {
    let start = Instant::now();
    let mut read = 0usize;
    while read < frame.len() {
        match stream.read(&mut frame[read..]) {
            Ok(0) => anyhow::bail!("CC connection closed while reading frame"),
            Ok(count) => read += count,
            Err(err) if retryable_cc_io(&err) && start.elapsed() < CC_FRAME_DEADLINE => {}
            Err(err) if retryable_cc_io(&err) => {
                anyhow::bail!("timed out reading CC frame after {} bytes", read)
            }
            Err(err) => return Err(err).context("failed to read CC frame"),
        }
    }
    Ok(())
}

fn wait_for_guest_console_login_via_cc(
    cc_sock: &Path,
    guest_handle: u32,
    guest_os: &str,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    let mut cc = connect_cc_client(cc_sock, timeout.min(Duration::from_secs(30)), qemu)?;
    wait_for_guest_console_login_on_cc(cc_sock, &mut cc, guest_handle, guest_os, timeout, qemu)
}

fn wait_for_guest_console_login_on_cc(
    cc_sock: &Path,
    cc: &mut CcClient,
    guest_handle: u32,
    guest_os: &str,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    let start = Instant::now();
    let mut transcript = String::new();
    let mut matched_prompt = None;
    let prompt_markers = guest_prompt_markers(guest_os);
    let mut freebsd_console_type_accepted = false;
    let mut freebsd_installer_shell_requested = false;
    let mut freebsd_rescue_shell_requested = false;
    let mut freebsd_stack_requested = false;
    let mut last_progress = Instant::now();

    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "waiting for guest login prompt via CC-PD API")?;
        let mut drained_console = false;
        match cc_log_stream_for_handle(cc, guest_handle, guest_os) {
            Ok(chunk) => {
                if !chunk.is_empty() {
                    drained_console = true;
                    transcript.push_str(&chunk);
                    if guest_os == "freebsd"
                        && !freebsd_rescue_shell_requested
                        && freebsd_static_rescue_needed(&transcript)
                    {
                        println!(
                            "[xtask:test] FreeBSD dynamic shell unavailable; selecting /rescue/sh"
                        );
                        cc_send_raw_bytes(cc, guest_handle, b"/rescue/sh\r")?;
                        freebsd_rescue_shell_requested = true;
                    }
                    reject_bad_guest_path(guest_os, &transcript)?;
                    if last_progress.elapsed() >= Duration::from_secs(30) {
                        println!(
                            "[xtask:test] {} console progress ({} bytes), tail:\n{}",
                            guest_os,
                            transcript.len(),
                            tail_chars(&transcript, 800)
                        );
                        last_progress = Instant::now();
                    }
                    if guest_os == "freebsd"
                        && !freebsd_console_type_accepted
                        && transcript.contains("Console type [vt100]:")
                    {
                        println!(
                            "[xtask:test] FreeBSD console type prompt reached; accepting vt100"
                        );
                        cc_send_raw_byte(cc, guest_handle, b'\r')?;
                        freebsd_console_type_accepted = true;
                    }
                    if guest_os == "freebsd"
                        && !freebsd_installer_shell_requested
                        && transcript.contains("FreeBSD Installer")
                        && transcript.contains("begin an installation or use the live")
                    {
                        println!("[xtask:test] FreeBSD installer menu reached; selecting shell");
                        cc_send_raw_bytes(cc, guest_handle, b"\t\r")?;
                        freebsd_installer_shell_requested = true;
                    }

                    let matched = if let Some(marker) = prompt_markers
                        .iter()
                        .find(|marker| transcript.contains(**marker))
                    {
                        if guest_os == "ubuntu-live" && !transcript.contains("Ubuntu 26.04") {
                            None
                        } else {
                            Some((*marker).to_string())
                        }
                    } else if guest_os == "freebsd"
                        && (freebsd_installer_shell_requested || freebsd_rescue_shell_requested)
                        && freebsd_shell_prompt_seen(&transcript)
                    {
                        Some(String::from("installer shell prompt"))
                    } else {
                        None
                    };

                    if let Some(marker) = matched {
                        matched_prompt = Some(marker);
                        break;
                    }
                }
            }
            Err(err) => {
                if cc.is_closed() {
                    return Err(err).context("CC console transport closed");
                }
                println!("[xtask:test] CC console drain not ready yet: {err:#}");
            }
        }
        if guest_os == "freebsd"
            && !freebsd_stack_requested
            && start.elapsed() >= Duration::from_secs(180)
        {
            println!("[xtask:test] requesting FreeBSD PID 1 stack");
            cc_send_raw_byte(cc, guest_handle, 0x1d)?;
            freebsd_stack_requested = true;
        }
        std::thread::sleep(if drained_console {
            Duration::from_millis(10)
        } else {
            Duration::from_millis(250)
        });
    }

    let prompt = matched_prompt.ok_or_else(|| {
        anyhow::anyhow!(
            "timed out after {}s waiting for {:?} via CC-PD console API; tail:\n{}",
            timeout.as_secs(),
            prompt_markers,
            tail_chars(&transcript, 4000)
        )
    })?;

    println!("[xtask:test] CC console matched prompt marker {:?}", prompt);

    let proof = verify_guest_console_input(
        cc_sock,
        cc,
        guest_handle,
        guest_os,
        timeout
            .saturating_sub(start.elapsed())
            .min(Duration::from_secs(if guest_os == "ubuntu-live" {
                360
            } else {
                20
            })),
        qemu,
    )?;
    Ok(format!(
        "CC console API saw {guest_os} handle {guest_handle} prompt {:?} and {proof}",
        prompt
    ))
}

fn guest_prompt_markers(guest_os: &str) -> &'static [&'static str] {
    match guest_os {
        "ubuntu" => &["agentos-linux login:", "ubuntu login:", "login:"],
        "ubuntu-live" => &["ubuntu login:", "login:"],
        "freebsd" => &["login:"],
        _ => &["login:"],
    }
}

fn reject_bad_guest_path(guest_os: &str, transcript: &str) -> anyhow::Result<()> {
    if guest_os == "ubuntu-live" && transcript.contains("Initramfs unpacking failed") {
        anyhow::bail!(
            "Ubuntu live initramfs did not unpack cleanly; tail:\n{}",
            tail_chars(transcript, 4000)
        );
    }
    if guest_os.starts_with("ubuntu")
        && (transcript.contains("emergency mode")
            || transcript.contains("Emergency Shell")
            || transcript.contains("Press Enter for maintenance"))
    {
        anyhow::bail!(
            "Ubuntu reached an emergency or maintenance path instead of multi-user login; tail:\n{}",
            tail_chars(transcript, 4000)
        );
    }
    if guest_os == "freebsd"
        && (transcript.contains("mountroot>")
            || transcript.contains("Manual root filesystem specification")
            || transcript.contains("Mounting from cd9660:")
                && transcript.contains("failed with error"))
    {
        anyhow::bail!(
            "FreeBSD reached mountroot, maintenance, or single-user fallback instead of a normal login or configured installer shell; tail:\n{}",
            tail_chars(transcript, 4000)
        );
    }
    Ok(())
}

fn freebsd_static_rescue_needed(transcript: &str) -> bool {
    transcript.contains("Enter full pathname of shell")
        && (transcript.contains("Unsupported version") || transcript.contains("ld-elf.so.1:"))
}

fn freebsd_shell_prompt_seen(transcript: &str) -> bool {
    transcript.contains("\n# ") || transcript.contains("\r# ") || transcript.ends_with("# ")
}

fn verify_guest_console_input(
    cc_sock: &Path,
    cc: &mut CcClient,
    guest_handle: u32,
    guest_os: &str,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    if guest_os == "ubuntu-live" {
        return verify_ubuntu_live_console_and_net(cc_sock, cc, guest_handle, timeout, qemu);
    }

    let probe = if guest_os.starts_with("ubuntu") {
        "agentos-linux-proof\n"
    } else {
        "~"
    };
    cc_send_raw_bytes(cc, guest_handle, probe.as_bytes())?;

    let mut echo = String::new();
    let start = Instant::now();
    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "waiting for guest console input echo via CC-PD API")?;
        let chunk = match cc_log_stream_for_handle(cc, guest_handle, guest_os) {
            Ok(chunk) => chunk,
            Err(err) => {
                if cc.is_closed() {
                    return Err(err).context("CC console transport closed after input");
                }
                println!("[xtask:test] CC console post-input drain not ready yet: {err:#}");
                String::new()
            }
        };
        if !chunk.is_empty() {
            echo.push_str(&chunk);
            if echo.contains(probe.trim_end()) {
                if guest_os != "ubuntu" {
                    let _ = cc_send_raw_byte(cc, guest_handle, 0x15); /* Ctrl-U */
                }
                return Ok(format!("guest echoed {:?}", probe.trim_end()));
            }
        }
        std::thread::sleep(Duration::from_millis(500));
    }

    anyhow::bail!(
        "guest reached prompt, but did not echo input {:?}; post-input tail:\n{}",
        probe.trim_end(),
        tail_chars(&echo, 2000)
    );
}

fn ubuntu_live_network_probe_command() -> &'static str {
    "sudo -n ip link set eth0 up && { ping -6 -c1 -W1 ff02::1%eth0 >/dev/null 2>&1 || true; } && printf 'agentos-live-net-%s\\n' proof"
}

fn verify_ubuntu_live_console_and_net(
    _cc_sock: &Path,
    cc: &mut CcClient,
    guest_handle: u32,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    cc_send_console_line(cc, guest_handle, b"ubuntu")?;

    let phase_timeout = timeout.min(Duration::from_secs(180));
    let login_start = Instant::now();
    let mut transcript = String::new();
    let mut blank_password_sent = false;
    let mut login_attempts = 1u32;
    while login_start.elapsed() < phase_timeout {
        ensure_qemu_running(qemu, "logging into Ubuntu live console")?;
        let chunk = match cc_log_stream_for_handle(cc, guest_handle, "ubuntu-live") {
            Ok(chunk) => chunk,
            Err(err) => {
                if cc.is_closed() {
                    return Err(err).context("CC live-login transport closed");
                }
                println!("[xtask:test] CC live-login drain not ready yet: {err:#}");
                String::new()
            }
        };
        if !chunk.is_empty() {
            transcript.push_str(&chunk);
            if transcript.contains("login: timed out")
                && transcript.to_ascii_lowercase().contains("ubuntu login:")
            {
                anyhow::ensure!(
                    login_attempts < 5,
                    "Ubuntu live console repeatedly dropped the login terminator; tail:\n{}",
                    tail_chars(&transcript, 2000)
                );
                cc_send_console_line(cc, guest_handle, b"ubuntu")?;
                login_attempts += 1;
                transcript.clear();
                blank_password_sent = false;
                continue;
            }
            if transcript.contains("Login incorrect") {
                if transcript.contains("pam_nologin") || transcript.contains("System is booting up")
                {
                    anyhow::ensure!(
                        login_attempts < 5,
                        "Ubuntu live user sessions never became available; tail:\n{}",
                        tail_chars(&transcript, 2000)
                    );
                    std::thread::sleep(Duration::from_secs(5));
                    cc_send_console_line(cc, guest_handle, b"ubuntu")?;
                    login_attempts += 1;
                    transcript.clear();
                    blank_password_sent = false;
                    continue;
                }
                anyhow::bail!(
                    "Ubuntu live account rejected console login; tail:\n{}",
                    tail_chars(&transcript, 2000)
                );
            }
            if !blank_password_sent && transcript.to_ascii_lowercase().contains("password:") {
                cc_send_raw_byte(cc, guest_handle, b'\r')?;
                blank_password_sent = true;
            }
            if transcript.contains("ubuntu@") && transcript.contains("$ ") {
                break;
            }
        }
        std::thread::sleep(Duration::from_millis(500));
    }

    anyhow::ensure!(
        transcript.contains("ubuntu@") && transcript.contains("$ "),
        "Ubuntu live login did not reach a shell prompt; tail:\n{}",
        tail_chars(&transcript, 2000)
    );

    /* Split the token so terminal command echo cannot satisfy the proof. */
    cc_send_console_line(cc, guest_handle, b"printf 'agentos-live-%s\\n' proof")?;
    let proof_start = Instant::now();
    let mut output = String::new();
    let mut userspace_proven = false;
    while proof_start.elapsed() < phase_timeout {
        ensure_qemu_running(qemu, "waiting for Ubuntu live userspace proof")?;
        let chunk = cc_log_stream_for_handle(cc, guest_handle, "ubuntu-live").unwrap_or_default();
        if !chunk.is_empty() {
            output.push_str(&chunk);
            if output.contains("agentos-live-proof") {
                userspace_proven = true;
                break;
            }
        }
        std::thread::sleep(Duration::from_millis(500));
    }

    anyhow::ensure!(
        userspace_proven,
        "Ubuntu live shell did not complete the userspace proof; login tail:\n{}\ncommand tail:\n{}",
        tail_chars(&transcript, 2000),
        tail_chars(&output, 2000),
    );

    /*
     * Link-up emits IPv6 control traffic; the bounded all-nodes ping guarantees
     * a guest-originated frame without DHCP. Wait for a marker emitted after
     * both operations so no probe bytes remain queued when SSH provisioning
     * starts on the same console.
     */
    cc_send_console_line(
        cc,
        guest_handle,
        ubuntu_live_network_probe_command().as_bytes(),
    )?;
    let network_start = Instant::now();
    let mut network_output = String::new();
    while network_start.elapsed() < phase_timeout {
        ensure_qemu_running(qemu, "waiting for Ubuntu live network proof")?;
        let chunk = match cc_log_stream_for_handle(cc, guest_handle, "ubuntu-live") {
            Ok(chunk) => chunk,
            Err(err) => {
                if cc.is_closed() {
                    return Err(err).context("CC live-network transport closed");
                }
                println!("[xtask:test] CC live-network drain not ready yet: {err:#}");
                String::new()
            }
        };
        if !chunk.is_empty() {
            network_output.push_str(&chunk);
            if network_output.contains("agentos-live-net-proof") {
                return Ok(String::from(
                    "logged into Ubuntu live userspace, executed a command, and emitted a network probe",
                ));
            }
        }
        std::thread::sleep(Duration::from_millis(500));
    }

    anyhow::bail!(
        "Ubuntu live shell did not complete the network proof; tail:\n{}",
        tail_chars(&network_output, 2000)
    )
}

fn try_create_guest_via_cc(
    cc: &mut CcClient,
    os_type: u8,
    ram_mb: u32,
) -> anyhow::Result<Result<u32, (u32, u32)>> {
    let mut shmem = [0u8; 52];
    shmem[0] = os_type;
    shmem[1] = VIBEOS_ARCH_AARCH64;
    wr32(&mut shmem, 4, ram_mb);
    wr32(
        &mut shmem,
        16,
        VIBEOS_DEV_SERIAL | VIBEOS_DEV_NET | VIBEOS_DEV_BLOCK,
    );

    let reply = cc
        .call(MSG_CC_CREATE_GUEST, 0, 0, 0, &shmem)
        .context("MSG_CC_CREATE_GUEST failed")?;
    if reply.mr[0] == CC_OK {
        Ok(Ok(reply.mr[1]))
    } else {
        Ok(Err((reply.mr[0], reply.mr[1])))
    }
}

fn create_guest_via_cc_wait(
    cc: &mut CcClient,
    os_type: u8,
    ram_mb: u32,
    label: &str,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<u32> {
    let start = Instant::now();
    let mut attempts = 0u32;
    let mut last_err = String::from("no create attempt completed");

    while start.elapsed() < timeout {
        attempts += 1;
        ensure_qemu_running(qemu, &format!("creating {label} guest through CC-PD"))?;

        match try_create_guest_via_cc(cc, os_type, ram_mb) {
            Ok(Ok(handle)) => {
                println!(
                    "[xtask:test] created {label} guest handle={handle} after {attempts} attempt(s)"
                );
                return Ok(handle);
            }
            Ok(Err((status, detail))) => {
                last_err = format!("MSG_CC_CREATE_GUEST returned ok={status} detail={detail}");
                if status != CC_ERR_RELAY_FAULT {
                    anyhow::bail!("failed to create {label} guest: {last_err}");
                }
                if attempts == 1 || attempts % 5 == 0 {
                    println!("[xtask:test] {label} guest create not ready yet: {last_err}");
                }
            }
            Err(err) => {
                last_err = format!("{err:#}");
                if cc.is_closed() {
                    return Err(err).context(format!("{label} guest create transport closed"));
                }
                if attempts == 1 || attempts % 5 == 0 {
                    println!(
                        "[xtask:test] {label} guest create transport not ready yet: {last_err}"
                    );
                }
            }
        }

        std::thread::sleep(Duration::from_secs(1));
    }

    anyhow::bail!(
        "timed out after {}s creating {label} guest through CC-PD/vm_manager; last error: {last_err}",
        timeout.as_secs()
    );
}

fn run_guest_console_command(
    _cc_sock: &Path,
    cc: &mut CcClient,
    guest_handle: u32,
    guest_os: &str,
    command: &str,
    marker: &str,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<()> {
    /*
     * Do not issue a speculative log drain here.  Immediately after a guest
     * handoff that extra synchronous RPC can consume the entire frame deadline
     * and close the retained CC session before any input is sent.  Each command
     * has a fresh success marker, so stale console output cannot satisfy it.
     */
    cc_send_console_line(cc, guest_handle, command.as_bytes())?;

    let start = Instant::now();
    let mut output = String::new();
    let failure_marker = format!("agentos-{guest_os}-ssh-failed");
    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "waiting for guest provisioning command")?;
        match cc_log_stream_for_handle(cc, guest_handle, guest_os) {
            Ok(chunk) => {
                output.push_str(&chunk);
                if output.contains(marker) {
                    return Ok(());
                }
                if output.contains(&failure_marker) {
                    anyhow::bail!(
                        "{guest_os} SSH provisioning reported failure; tail:\n{}",
                        tail_chars(&output, 3000)
                    );
                }
            }
            Err(err) => {
                if cc.is_closed() {
                    return Err(err).context("CC provisioning transport closed");
                }
                println!("[xtask:test] CC provisioning poll not ready yet: {err:#}");
            }
        }
        std::thread::sleep(Duration::from_millis(250));
    }
    anyhow::bail!(
        "{guest_os} SSH provisioning command did not emit {marker:?}; tail:\n{}",
        tail_chars(&output, 3000)
    );
}

fn run_guest_console_commands(
    cc_sock: &Path,
    cc: &mut CcClient,
    guest_handle: u32,
    guest_os: &str,
    commands: &[String],
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<()> {
    anyhow::ensure!(
        !commands.is_empty(),
        "guest provisioning command list is empty"
    );
    for (index, command) in commands.iter().enumerate() {
        println!(
            "[xtask:test] provisioning {guest_os} over CC console (step {}/{})",
            index + 1,
            commands.len()
        );
        let last = index + 1 == commands.len();
        let success = if last {
            "ready".to_string()
        } else {
            format!("step-{}", index + 1)
        };
        let marker = format!("agentos-{guest_os}-ssh-{success}");
        let wrapped = guest_provision_command(command, guest_os, &success);
        run_guest_console_command(
            cc_sock,
            cc,
            guest_handle,
            guest_os,
            &wrapped,
            &marker,
            timeout,
            qemu,
        )?;
    }
    Ok(())
}

fn guest_provision_command(command: &str, guest_os: &str, success: &str) -> String {
    format!(
        "({command}) && printf 'agentos-{guest_os}-ssh-%s\\n' '{success}' || printf 'agentos-{guest_os}-ssh-%s\\n' failed"
    )
}

fn provision_ubuntu_and_resume_freebsd_ssh(
    cc_sock: &Path,
    cc: &mut CcClient,
    linux_handle: u32,
    freebsd_handle: u32,
    ssh_key: &SshTestKey,
    qemu: &mut Child,
) -> anyhow::Result<()> {
    let ubuntu = ubuntu_ssh_provision_commands(&ssh_key.public_key);
    run_guest_console_commands(
        cc_sock,
        cc,
        linux_handle,
        "ubuntu",
        &ubuntu,
        Duration::from_secs(600),
        qemu,
    )?;

    /*
     * FreeBSD was provisioned before its boot checkpoint. Resume it only
     * after Ubuntu provisioning is complete, then prove that its sshd and
     * network configuration survived the suspend/resume boundary.
     */
    let freebsd_provision_resume = resume_guest_via_cc(cc, freebsd_handle)
        .context("failed to resume provisioned FreeBSD for concurrent SSH proof")?;
    println!(
        "[xtask:test] resumed provisioned FreeBSD guest handle={freebsd_handle} state={freebsd_provision_resume} for concurrent SSH proof"
    );
    Ok(())
}

fn ubuntu_ssh_provision_commands(public_key: &str) -> Vec<String> {
    vec![
        "sudo -n ip link set eth0 up && sudo -n ip addr flush dev eth0 scope global && sudo -n ip addr add 10.0.2.15/24 dev eth0 && sudo -n ip route replace default via 10.0.2.2".into(),
        "sudo -n rm -f /etc/resolv.conf && printf 'nameserver 10.0.2.3\\n' | sudo -n tee /etc/resolv.conf >/dev/null".into(),
        "if command -v /usr/sbin/sshd >/dev/null 2>&1; then true; else ok=1; for deb in /cdrom/pool/main/o/openssh/openssh-sftp-server_*.deb /cdrom/pool/main/o/openssh/openssh-server_*.deb; do test -f \"$deb\" && sudo -n dpkg-deb -x \"$deb\" / || ok=0; done; test \"$ok\" -eq 1 && command -v /usr/sbin/sshd >/dev/null; fi".into(),
        "getent passwd sshd >/dev/null || sudo -n useradd --system --home /run/sshd --shell /usr/sbin/nologin sshd".into(),
        format!("sudo -n mkdir -p /home/ubuntu/.ssh /run/sshd && printf '%s\\n' '{}' | sudo -n tee /home/ubuntu/.ssh/authorized_keys >/dev/null && sudo -n chown -R ubuntu:ubuntu /home/ubuntu/.ssh && sudo -n chmod 700 /home/ubuntu/.ssh && sudo -n chmod 600 /home/ubuntu/.ssh/authorized_keys", public_key),
        "sudo -n rm -f /run/agentos-ssh-host-key /run/agentos-ssh-host-key.pub /run/agentos-sshd.pid && sudo -n ssh-keygen -q -t ed25519 -N '' -f /run/agentos-ssh-host-key".into(),
        "sudo -n /usr/sbin/sshd -t -f /dev/null -o HostKey=/run/agentos-ssh-host-key -o AuthorizedKeysFile=/home/ubuntu/.ssh/authorized_keys -o UsePAM=no -o PidFile=/run/agentos-sshd.pid".into(),
        "sudo -n /usr/sbin/sshd -f /dev/null -o HostKey=/run/agentos-ssh-host-key -o AuthorizedKeysFile=/home/ubuntu/.ssh/authorized_keys -o UsePAM=no -o UseDNS=no -o GSSAPIAuthentication=no -o MaxStartups=100:100:100 -o PidFile=/run/agentos-sshd.pid -o PasswordAuthentication=no -o KbdInteractiveAuthentication=no -o PubkeyAuthentication=yes -o PermitRootLogin=no".into(),
    ]
}

fn freebsd_ssh_provision_commands(public_key: &str) -> Vec<String> {
    vec![
        "mkdir -p /tmp/agentos-ssh 2>/dev/null || { mount -t tmpfs tmpfs /tmp && mkdir -p /tmp/agentos-ssh; }".into(),
        format!("rm -f /tmp/agentos-ssh/host_key /tmp/agentos-ssh/host_key.pub /tmp/agentos-ssh/sshd.pid && printf '%s\\n' '{}' > /tmp/agentos-ssh/authorized_keys && chmod 600 /tmp/agentos-ssh/authorized_keys", public_key),
        "ifconfig vtnet0 inet 10.0.2.16 netmask 255.255.255.0 up && ( route delete default >/dev/null 2>&1 || true ) && ( route add default 10.0.2.2 >/dev/null 2>&1 || true )".into(),
        "ssh-keygen -q -t ed25519 -N '' -f /tmp/agentos-ssh/host_key".into(),
        "/usr/sbin/sshd -t -f /dev/null -o HostKey=/tmp/agentos-ssh/host_key -o AuthorizedKeysFile=/tmp/agentos-ssh/authorized_keys -o StrictModes=no -o PermitRootLogin=yes -o PasswordAuthentication=no -o KbdInteractiveAuthentication=no -o PubkeyAuthentication=yes -o UsePAM=no -o PidFile=/tmp/agentos-ssh/sshd.pid".into(),
        "/usr/sbin/sshd -f /dev/null -o HostKey=/tmp/agentos-ssh/host_key -o AuthorizedKeysFile=/tmp/agentos-ssh/authorized_keys -o StrictModes=no -o PermitRootLogin=yes -o PasswordAuthentication=no -o KbdInteractiveAuthentication=no -o PubkeyAuthentication=yes -o UsePAM=no -o PidFile=/tmp/agentos-ssh/sshd.pid".into(),
    ]
}

fn ubuntu_desktop_provision_script() -> &'static str {
    r#"set -eu
TIGERVNC_DEB=/tmp/tigervnc-standalone-server.deb
TIGERVNC_URL=http://archive.ubuntu.com/ubuntu/pool/universe/t/tigervnc/tigervnc-standalone-server_1.15.0+dfsg-2build1_arm64.deb
TIGERVNC_SHA256=30e536f05a504ad817b294abee51394143e140c57e4b4cea43c149973cfba3b2
if ! command -v Xtigervnc >/dev/null 2>&1; then
    TIGERVNC_COMMON_DEB=/tmp/tigervnc-common.deb
    TIGERVNC_COMMON_URL=http://archive.ubuntu.com/ubuntu/pool/universe/t/tigervnc/tigervnc-common_1.15.0+dfsg-2build1_arm64.deb
    TIGERVNC_COMMON_SHA256=ee669c6253bde0b9f7b43c270607d560ea27285e88ba7dc379338e1cf91c3d1e
    timeout 600 wget -q -O "$TIGERVNC_COMMON_DEB" "$TIGERVNC_COMMON_URL"
    printf '%s  %s\n' "$TIGERVNC_COMMON_SHA256" "$TIGERVNC_COMMON_DEB" | sha256sum -c -
    timeout 600 wget -q -O "$TIGERVNC_DEB" "$TIGERVNC_URL"
    printf '%s  %s\n' "$TIGERVNC_SHA256" "$TIGERVNC_DEB" | sha256sum -c -
    dpkg-deb -x "$TIGERVNC_COMMON_DEB" /
    dpkg-deb -x "$TIGERVNC_DEB" /
fi
command -v Xtigervnc >/dev/null
command -v gnome-calculator >/dev/null
printf 'agentos-desktop-binaries-ready\n'
install -d -o ubuntu -g ubuntu /home/ubuntu/.vnc
rm -f /tmp/.X1-lock /tmp/.X11-unix/X1 /tmp/agentos-desktop.pid
setsid -f su -s /bin/sh ubuntu -c 'exec env HOME=/home/ubuntu USER=ubuntu Xtigervnc :1 -rfbport 5901 -rendernode "" -ac -localhost yes -SecurityTypes None -geometry 1024x768 -depth 24' </dev/null >/tmp/agentos-xvnc.log 2>&1
attempt=0
while test "$attempt" -lt 120; do
    test -S /tmp/.X11-unix/X1 && ss -ltn | grep -q ':5901' && break
    attempt=$((attempt + 1))
    sleep 1
done
if ! test -S /tmp/.X11-unix/X1 || ! ss -ltn | grep -q ':5901'; then
    cat /tmp/agentos-xvnc.log >&2
    exit 1
fi
printf 'agentos-desktop-rfb-listener-ready\n'
su -s /bin/sh ubuntu -c 'env HOME=/home/ubuntu USER=ubuntu DISPLAY=:1 dbus-run-session -- gnome-calculator >/tmp/agentos-desktop.log 2>&1 & echo $! >/tmp/agentos-desktop.pid'
attempt=0
while test "$attempt" -lt 120; do
    if pgrep -x Xtigervnc >/dev/null &&
       test -S /tmp/.X11-unix/X1 &&
       ss -ltn | grep -q ':5901' &&
       test -s /tmp/agentos-desktop.pid &&
       kill -0 "$(cat /tmp/agentos-desktop.pid)" 2>/dev/null; then
        printf 'agentos-desktop-app-ready\n'
        break
    fi
    attempt=$((attempt + 1))
    sleep 1
done
if ! pgrep -x Xtigervnc >/dev/null ||
   ! test -S /tmp/.X11-unix/X1 ||
   ! ss -ltn | grep -q ':5901' ||
   ! test -s /tmp/agentos-desktop.pid ||
   ! kill -0 "$(cat /tmp/agentos-desktop.pid)" 2>/dev/null; then
    cat /tmp/agentos-xvnc.log /tmp/agentos-desktop.log >&2
    exit 1
fi
RFB_VERSION="$(timeout 120 bash -c 'exec 3<>/dev/tcp/127.0.0.1/5901; IFS= read -r version <&3; printf "%s" "$version"')"
if test "$RFB_VERSION" != 'RFB 003.008'; then
    printf 'unexpected local RFB version: %s\n' "$RFB_VERSION" >&2
    cat /tmp/agentos-xvnc.log /tmp/agentos-desktop.log >&2
    exit 1
fi
printf 'agentos-desktop-local-rfb-ready\n'
"#
}

fn run_ssh_script(private_key: &Path, script: &str) -> anyhow::Result<String> {
    let mut child = std::process::Command::new("ssh");
    child
        .arg("-i")
        .arg(private_key)
        .args(["-p", &UBUNTU_DEFAULT_SSH_PORT.to_string()])
        .args(SSH_AUTH_OPTIONS)
        .args(SSH_SESSION_LIVENESS_OPTIONS)
        .args(["ubuntu@127.0.0.1", "sudo -n timeout 1200 sh -s"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let mut child = child
        .spawn()
        .context("failed to launch Ubuntu desktop provisioning over SSH")?;
    child
        .stdin
        .take()
        .context("desktop provisioning SSH stdin was not piped")?
        .write_all(script.as_bytes())
        .context("failed to send Ubuntu desktop provisioning script")?;
    let output = child
        .wait_with_output()
        .context("failed to wait for Ubuntu desktop provisioning")?;
    anyhow::ensure!(
        output.status.success(),
        "Ubuntu desktop provisioning failed with {}: stdout={:?} stderr={:?}",
        output.status,
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    Ok(String::from_utf8_lossy(&output.stdout).into_owned())
}

fn wait_for_ubuntu_ssh(
    ssh_key: &SshTestKey,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<()> {
    let start = Instant::now();
    let mut last = String::from("no SSH attempt completed");
    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "waiting for Ubuntu desktop SSH")?;
        let probe = spawn_ssh_probe(&ssh_key.private_key, UBUNTU_DEFAULT_SSH_PORT, "ubuntu")?;
        let output = probe
            .wait_with_output()
            .context("failed to wait for Ubuntu desktop SSH probe")?;
        if output.status.success() && String::from_utf8_lossy(&output.stdout).trim() == "Linux" {
            return Ok(());
        }
        last = format!(
            "status={} stdout={:?} stderr={:?}",
            output.status,
            String::from_utf8_lossy(&output.stdout).trim(),
            String::from_utf8_lossy(&output.stderr).trim()
        );
        std::thread::sleep(Duration::from_secs(2));
    }
    anyhow::bail!("Ubuntu desktop SSH did not become ready: {last}")
}

fn desktop_tunnel_forward_spec() -> String {
    format!("127.0.0.1:{UBUNTU_DESKTOP_PORT}:127.0.0.1:{UBUNTU_VNC_GUEST_PORT}")
}

fn spawn_desktop_tunnel(private_key: &Path) -> anyhow::Result<Child> {
    std::process::Command::new("ssh")
        .arg("-i")
        .arg(private_key)
        .args(["-p", &UBUNTU_DEFAULT_SSH_PORT.to_string()])
        .args(SSH_AUTH_OPTIONS)
        .args(SSH_SESSION_LIVENESS_OPTIONS)
        .args([
            "-o",
            "ExitOnForwardFailure=yes",
            "-N",
            "-L",
            &desktop_tunnel_forward_spec(),
            "ubuntu@127.0.0.1",
        ])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .context("failed to launch SSH tunnel for Ubuntu desktop")
}

fn prove_ubuntu_desktop(
    cc_sock: &Path,
    ssh_key: &SshTestKey,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<(rfb::RfbFrameEvidence, Child)> {
    let mut cc = connect_cc_client(cc_sock, timeout.min(Duration::from_secs(30)), qemu)?;
    let provision_ssh = ubuntu_ssh_provision_commands(&ssh_key.public_key);
    run_guest_console_commands(
        cc_sock,
        &mut cc,
        0,
        "ubuntu",
        &provision_ssh,
        timeout.min(Duration::from_secs(600)),
        qemu,
    )?;
    drop(cc);
    wait_for_ubuntu_ssh(ssh_key, timeout.min(Duration::from_secs(600)), qemu)?;
    let provisioning = run_ssh_script(&ssh_key.private_key, ubuntu_desktop_provision_script())?;
    println!("[xtask:test] Ubuntu desktop provisioning:\n{provisioning}");

    let mut tunnel = spawn_desktop_tunnel(&ssh_key.private_key)?;
    let start = Instant::now();
    let mut last = String::from("SSH tunnel did not accept a connection");
    while start.elapsed() < timeout.min(UBUNTU_DESKTOP_RFB_TIMEOUT) {
        ensure_qemu_running(qemu, "waiting for Ubuntu desktop RFB frame")?;
        if let Some(status) = tunnel
            .try_wait()
            .context("failed to inspect Ubuntu desktop SSH tunnel")?
        {
            let mut stderr = String::new();
            if let Some(mut pipe) = tunnel.stderr.take() {
                let _ = pipe.read_to_string(&mut stderr);
            }
            anyhow::bail!("Ubuntu desktop SSH tunnel exited with {status}: {stderr}");
        }
        match TcpStream::connect(SocketAddr::from(([127, 0, 0, 1], UBUNTU_DESKTOP_PORT))) {
            Ok(mut stream) => {
                stream
                    .set_read_timeout(Some(UBUNTU_DESKTOP_RFB_IO_TIMEOUT))
                    .context("failed to bound desktop RFB reads")?;
                stream
                    .set_write_timeout(Some(UBUNTU_DESKTOP_RFB_IO_TIMEOUT))
                    .context("failed to bound desktop RFB writes")?;
                match rfb::verify_raw_frame(&mut stream) {
                    Ok(evidence) => return Ok((evidence, tunnel)),
                    Err(error) => last = format!("{error:#}"),
                }
            }
            Err(error) => last = error.to_string(),
        }
        std::thread::sleep(Duration::from_secs(1));
    }

    let _ = tunnel.kill();
    let _ = tunnel.wait();
    let mut tunnel_stderr = String::new();
    if let Some(mut pipe) = tunnel.stderr.take() {
        let _ = pipe.read_to_string(&mut tunnel_stderr);
    }
    anyhow::bail!(
        "Ubuntu desktop did not yield an RFB frame: {last}; SSH tunnel stderr={:?}",
        tunnel_stderr.trim()
    )
}

fn spawn_ssh_probe(
    private_key: &Path,
    port: u16,
    user: &str,
) -> anyhow::Result<std::process::Child> {
    std::process::Command::new("ssh")
        .args([
            "-i",
            private_key
                .to_str()
                .context("SSH private key path is not UTF-8")?,
            "-p",
            &port.to_string(),
        ])
        .args(SSH_AUTH_OPTIONS)
        .args(SSH_PROBE_LIVENESS_OPTIONS)
        .args([&format!("{user}@127.0.0.1"), "uname -s"])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .with_context(|| format!("failed to launch SSH probe for {user} on port {port}"))
}

fn wait_for_dual_ssh(
    ssh_key: &SshTestKey,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    let start = Instant::now();
    let mut last = String::from("no SSH attempts completed");
    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "waiting for dual authenticated SSH")?;
        let ubuntu = spawn_ssh_probe(&ssh_key.private_key, UBUNTU_DEFAULT_SSH_PORT, "ubuntu")?;
        let freebsd = spawn_ssh_probe(&ssh_key.private_key, FREEBSD_DEFAULT_SSH_PORT, "root")?;
        let ubuntu_out = ubuntu
            .wait_with_output()
            .context("failed to wait for Ubuntu SSH probe")?;
        let freebsd_out = freebsd
            .wait_with_output()
            .context("failed to wait for FreeBSD SSH probe")?;
        let ubuntu_stdout = String::from_utf8_lossy(&ubuntu_out.stdout);
        let freebsd_stdout = String::from_utf8_lossy(&freebsd_out.stdout);
        if ubuntu_out.status.success()
            && freebsd_out.status.success()
            && ubuntu_stdout.trim() == "Linux"
            && freebsd_stdout.trim() == "FreeBSD"
        {
            return Ok(String::from(
                "concurrent authenticated SSH returned Linux and FreeBSD",
            ));
        }
        last = format!(
            "ubuntu status={} stdout={:?} stderr={:?}; freebsd status={} stdout={:?} stderr={:?}",
            ubuntu_out.status,
            ubuntu_stdout.trim(),
            String::from_utf8_lossy(&ubuntu_out.stderr).trim(),
            freebsd_out.status,
            freebsd_stdout.trim(),
            String::from_utf8_lossy(&freebsd_out.stderr).trim(),
        );
        std::thread::sleep(Duration::from_secs(2));
    }
    anyhow::bail!("dual authenticated SSH did not become ready: {last}")
}

fn wait_for_dual_guest_consoles_via_cc(
    cc_sock: &Path,
    timeout: Duration,
    qemu: &mut Child,
    ssh_key: &SshTestKey,
    keep_running: bool,
) -> anyhow::Result<String> {
    let start = Instant::now();
    let create_timeout = timeout;
    /*
     * VirtIO-console is a byte stream. Keep one connection across both
     * CREATE replies and the first SUSPEND so QEMU cannot still be retiring
     * a closed socket while the lifecycle request is already buffered on a
     * replacement connection.
     */
    let mut boot_cc = connect_cc_client(cc_sock, timeout.min(Duration::from_secs(30)), qemu)?;

    let freebsd_handle = create_guest_via_cc_wait(
        &mut boot_cc,
        VIBEOS_TYPE_FREEBSD,
        256,
        "FreeBSD",
        create_timeout,
        qemu,
    )
    .context("failed to create FreeBSD guest through vm_manager")?;

    let linux_handle = create_guest_via_cc_wait(
        &mut boot_cc,
        VIBEOS_TYPE_LINUX,
        1024,
        "Linux",
        create_timeout,
        qemu,
    )
    .context("failed to create Linux guest through vm_manager")?;

    /*
     * Creating both guests establishes both clients of the shared host block
     * path. Quiesce Ubuntu immediately so FreeBSD's ISO boot cannot lose the
     * single emulated CPU to the much busier live-image boot.
     */
    let linux_boot_suspend = suspend_guest_via_cc(&mut boot_cc, linux_handle)
        .context("failed to suspend Ubuntu while FreeBSD reaches its installer shell")?;
    println!(
        "[xtask:test] suspended Ubuntu guest handle={linux_handle} state={linux_boot_suspend} while FreeBSD boots"
    );

    let freebsd = wait_for_guest_console_login_on_cc(
        cc_sock,
        &mut boot_cc,
        freebsd_handle,
        "freebsd",
        timeout.saturating_sub(start.elapsed()),
        qemu,
    )?;
    /*
     * Provision FreeBSD while its installer shell is already responsive.
     * The later authenticated SSH probe then verifies that both its service
     * state and network configuration survive a real suspend/resume cycle.
     */
    let freebsd_ssh = freebsd_ssh_provision_commands(&ssh_key.public_key);
    run_guest_console_commands(
        cc_sock,
        &mut boot_cc,
        freebsd_handle,
        "freebsd",
        &freebsd_ssh,
        Duration::from_secs(600),
        qemu,
    )?;
    let freebsd_boot_suspend = suspend_guest_via_cc(&mut boot_cc, freebsd_handle)
        .context("failed to suspend provisioned FreeBSD guest while Ubuntu finishes booting")?;
    println!(
        "[xtask:test] suspended provisioned FreeBSD guest handle={freebsd_handle} state={freebsd_boot_suspend} while Ubuntu boots"
    );
    let linux_boot_resume = resume_guest_via_cc(&mut boot_cc, linux_handle)
        .context("failed to resume Ubuntu after checkpointing FreeBSD")?;
    println!("[xtask:test] resumed Ubuntu guest handle={linux_handle} state={linux_boot_resume}");

    let linux = wait_for_guest_console_login_on_cc(
        cc_sock,
        &mut boot_cc,
        linux_handle,
        "ubuntu-live",
        timeout.saturating_sub(start.elapsed()),
        qemu,
    )?;
    provision_ubuntu_and_resume_freebsd_ssh(
        cc_sock,
        &mut boot_cc,
        linux_handle,
        freebsd_handle,
        ssh_key,
        qemu,
    )?;
    let ssh = wait_for_dual_ssh(ssh_key, Duration::from_secs(600), qemu)?;

    if !keep_running {
        destroy_guest_via_cc(&mut boot_cc, linux_handle)
            .context("failed to destroy Linux guest")?;
        destroy_guest_via_cc(&mut boot_cc, freebsd_handle)
            .context("failed to destroy FreeBSD guest")?;
    }

    Ok(format!(
        "dual CC/vm_manager consoles ready: linux_handle={linux_handle} ({linux}); freebsd_handle={freebsd_handle} ({freebsd}); {ssh}; {}",
        if keep_running {
            "both retained for manual SSH"
        } else {
            "both destroyed"
        }
    ))
}

fn connect_cc_client(
    cc_sock: &Path,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<CcClient> {
    let start = Instant::now();
    let mut last_err = None;
    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "connecting to CC-PD socket")?;
        if cc_sock.exists() {
            match CcClient::connect(cc_sock) {
                Ok(cc) => return Ok(cc),
                Err(err) => last_err = Some(format!("{err:#}")),
            }
        }
        std::thread::sleep(Duration::from_millis(200));
    }

    if let Some(err) = last_err {
        anyhow::bail!(
            "failed to connect to {} within {}s: {}",
            cc_sock.display(),
            timeout.as_secs(),
            err
        );
    }
    anyhow::bail!(
        "CC-PD socket {} did not appear within {}s",
        cc_sock.display(),
        timeout.as_secs()
    );
}

pub fn wait_for_cc_socket(
    cc_sock: &Path,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<()> {
    let start = Instant::now();
    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "waiting for CC-PD socket")?;
        if cc_sock.exists() {
            return Ok(());
        }
        std::thread::sleep(Duration::from_millis(200));
    }

    anyhow::bail!(
        "CC-PD socket {} did not appear within {}s",
        cc_sock.display(),
        timeout.as_secs()
    );
}

fn ensure_qemu_running(qemu: &mut Child, context: &str) -> anyhow::Result<()> {
    if let Some(status) = qemu
        .try_wait()
        .with_context(|| format!("failed to poll QEMU status while {context}"))?
    {
        anyhow::bail!("QEMU exited with status {status} while {context}");
    }
    Ok(())
}

fn connect_host_net_stimulus(port: u16, qemu: &mut Child) -> Option<TcpStream> {
    let addr = SocketAddr::from(([127, 0, 0, 1], port));
    let start = Instant::now();
    while start.elapsed() < Duration::from_secs(10) {
        if ensure_qemu_running(qemu, "connecting host network stimulus").is_err() {
            return None;
        }
        if let Ok(stream) = TcpStream::connect_timeout(&addr, Duration::from_millis(250)) {
            println!("[xtask:test] Host network stimulus connected through 127.0.0.1:{port}");
            return Some(stream);
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    println!("[xtask:test] WARN: host network stimulus could not connect to 127.0.0.1:{port}");
    None
}

fn cc_log_stream_for_handle(
    cc: &mut CcClient,
    guest_handle: u32,
    guest_os: &str,
) -> anyhow::Result<String> {
    let pd_id = if guest_handle == 0 {
        0
    } else if guest_os == "freebsd" {
        TRACE_PD_FREEBSD_VMM
    } else {
        TRACE_PD_LINUX_VMM
    };
    let reply = cc
        .call(MSG_CC_LOG_STREAM, guest_handle, pd_id, 0, &[])
        .context("MSG_CC_LOG_STREAM failed")?;
    anyhow::ensure!(
        reply.mr[0] == CC_OK,
        "MSG_CC_LOG_STREAM returned ok={}",
        reply.mr[0]
    );
    let len = (reply.mr[1] as usize).min(reply.shmem.len());
    Ok(String::from_utf8_lossy(&reply.shmem[..len]).into_owned())
}

fn cc_send_raw_byte(cc: &mut CcClient, guest_handle: u32, byte: u8) -> anyhow::Result<()> {
    let mut shmem = [0u8; 24];
    wr32(&mut shmem, 0, CC_INPUT_KEY_DOWN);
    wr32(&mut shmem, 4, CC_INPUT_RAW_BYTE_BASE | u32::from(byte));

    cc_send_input_frame(cc, guest_handle, &shmem, "MSG_CC_SEND_INPUT")?;
    Ok(())
}

fn cc_text_event(chunk: &[u8]) -> Vec<u8> {
    let mut shmem = vec![0u8; 24 + chunk.len()];
    wr32(&mut shmem, 0, CC_INPUT_TEXT);
    wr32(&mut shmem, 4, chunk.len() as u32);
    shmem[24..].copy_from_slice(chunk);
    shmem
}

fn cc_send_raw_bytes(cc: &mut CcClient, guest_handle: u32, bytes: &[u8]) -> anyhow::Result<()> {
    for chunk in bytes.chunks(CC_INPUT_TEXT_CHUNK) {
        let shmem = cc_text_event(chunk);
        cc_send_input_frame(cc, guest_handle, &shmem, "MSG_CC_SEND_INPUT text")?;
        /*
         * Let the lower-priority guest consume RX descriptors between frames.
         * Without this yield, a host can fill the VMM ingress queue while the
         * higher-priority CC/Vibe/VM-manager call chain remains runnable.
         */
        std::thread::sleep(Duration::from_millis(50));
    }
    Ok(())
}

fn cc_send_console_line(cc: &mut CcClient, guest_handle: u32, line: &[u8]) -> anyhow::Result<()> {
    cc_send_raw_bytes(cc, guest_handle, line)?;
    cc_send_raw_byte(cc, guest_handle, b'\r')
}

fn cc_send_input_frame(
    cc: &mut CcClient,
    guest_handle: u32,
    shmem: &[u8],
    operation: &str,
) -> anyhow::Result<()> {
    let deadline = Instant::now() + CC_INPUT_RETRY_DEADLINE;
    loop {
        let reply = cc
            .call(MSG_CC_SEND_INPUT, guest_handle, 0, 0, shmem)
            .with_context(|| format!("{operation} failed"))?;
        if reply.mr[0] == CC_OK {
            return Ok(());
        }
        /*
         * CC currently folds a full downstream console queue into
         * CC_ERR_RELAY_FAULT. The VMM rejects before enqueue, so retrying the
         * same frame cannot duplicate input. Keep the retry bounded so a
         * permanent relay failure still terminates the acceptance gate.
         */
        if reply.mr[0] != CC_ERR_RELAY_FAULT || Instant::now() >= deadline {
            anyhow::bail!("{operation} returned ok={}", reply.mr[0]);
        }
        std::thread::sleep(Duration::from_millis(250));
    }
}

fn destroy_guest_via_cc(cc: &mut CcClient, guest_handle: u32) -> anyhow::Result<()> {
    let reply = cc
        .call(
            MSG_CC_DESTROY_GUEST,
            guest_handle,
            GUEST_DESTROY_NORMAL,
            0,
            &[],
        )
        .context("MSG_CC_DESTROY_GUEST failed")?;
    anyhow::ensure!(
        reply.mr[0] == CC_OK,
        "MSG_CC_DESTROY_GUEST returned ok={}",
        reply.mr[0]
    );
    Ok(())
}

fn lifecycle_guest_via_cc(
    cc: &mut CcClient,
    opcode: u32,
    guest_handle: u32,
    action: &str,
) -> anyhow::Result<u32> {
    let reply = cc
        .call(opcode, guest_handle, 0, 0, &[])
        .with_context(|| format!("MSG_CC_{action}_GUEST failed"))?;
    anyhow::ensure!(
        reply.mr[0] == CC_OK,
        "MSG_CC_{action}_GUEST returned ok={} detail={}",
        reply.mr[0],
        reply.mr[1]
    );
    Ok(reply.mr[1])
}

fn suspend_guest_via_cc(cc: &mut CcClient, guest_handle: u32) -> anyhow::Result<u32> {
    lifecycle_guest_via_cc(cc, MSG_CC_SUSPEND_GUEST, guest_handle, "SUSPEND")
}

fn resume_guest_via_cc(cc: &mut CcClient, guest_handle: u32) -> anyhow::Result<u32> {
    lifecycle_guest_via_cc(cc, MSG_CC_RESUME_GUEST, guest_handle, "RESUME")
}

pub fn cc_call(
    cc_sock: &Path,
    opcode: u32,
    mr1: u32,
    mr2: u32,
    mr3: u32,
    shmem_in: &[u8],
) -> anyhow::Result<CcReply> {
    CcClient::connect(cc_sock)?.call(opcode, mr1, mr2, mr3, shmem_in)
}

fn rd32(src: &[u8], off: usize) -> u32 {
    u32::from_le_bytes(src[off..off + 4].try_into().unwrap())
}

fn wr32(dst: &mut [u8], off: usize, value: u32) {
    dst[off..off + 4].copy_from_slice(&value.to_le_bytes());
}

fn tail_chars(s: &str, max_chars: usize) -> String {
    let len = s.chars().count();
    s.chars().skip(len.saturating_sub(max_chars)).collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::net::UnixListener;

    #[test]
    fn text_events_fit_the_narrowest_relay_and_reassemble() {
        let input = b"abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHI";
        let mut output = Vec::new();
        let mut frame_count = 0;
        for chunk in input.chunks(CC_INPUT_TEXT_CHUNK) {
            let event = cc_text_event(chunk);
            assert!(8 + event.len() <= VMM_RELAY_PAYLOAD_BYTES);
            assert_eq!(rd32(&event, 0), CC_INPUT_TEXT);
            assert_eq!(rd32(&event, 4) as usize, chunk.len());
            output.extend_from_slice(&event[24..]);
            frame_count += 1;
        }
        assert_eq!(frame_count, 3);
        assert_eq!(output, input);
    }

    #[test]
    fn cc_client_replays_identical_frame_after_lost_reply() {
        let dir = tempfile::tempdir().expect("temporary CC socket directory");
        let socket = dir.path().join("cc.sock");
        let listener = UnixListener::bind(&socket).expect("bind CC test socket");
        let server = std::thread::spawn(move || {
            let (mut first, _) = listener.accept().expect("accept first CC connection");
            let mut first_request = [0u8; CC_REQ_SIZE];
            first
                .read_exact(&mut first_request)
                .expect("read first CC request");
            drop(first);

            let (mut replay, _) = listener.accept().expect("accept replay CC connection");
            let mut replay_request = [0u8; CC_REQ_SIZE];
            replay
                .read_exact(&mut replay_request)
                .expect("read replayed CC request");
            assert_eq!(replay_request, first_request);

            let mut reply = [0u8; CC_REPLY_SIZE];
            wr32(&mut reply, 0, CC_OK);
            wr32(&mut reply, 4, 0x51a7e);
            replay.write_all(&reply).expect("write replayed CC reply");
        });

        let mut client = CcClient::connect(&socket).expect("connect CC test client");
        let reply = client
            .call(MSG_CC_SEND_INPUT, 7, 0, 0, b"lost-reply-regression")
            .expect("replay CC call after lost reply");
        assert_eq!(reply.mr[0], CC_OK);
        assert_eq!(reply.mr[1], 0x51a7e);
        server.join().expect("join CC test server");
    }

    #[test]
    fn dual_host_forwards_have_distinct_guest_addresses() {
        let netdev = dual_qemu_netdev_arg(UBUNTU_DEFAULT_SSH_PORT);
        assert!(netdev.contains("127.0.0.1:12222-10.0.2.15:22"));
        assert!(netdev.contains("127.0.0.1:12223-10.0.2.16:22"));
    }

    #[test]
    fn ssh_provisioning_commands_are_posix_shell_syntax() {
        let key = "ssh-ed25519 AAAAC3NzaFocusedTest agentos-test";
        let commands = ubuntu_ssh_provision_commands(key)
            .into_iter()
            .chain(freebsd_ssh_provision_commands(key));
        for command in commands {
            let status = std::process::Command::new("sh")
                .args(["-n", "-c", &command])
                .status()
                .expect("run sh syntax check");
            assert!(status.success());
        }
        assert!(ubuntu_ssh_provision_commands(key)
            .iter()
            .any(|command| command.contains(key)));
        assert!(freebsd_ssh_provision_commands(key)
            .iter()
            .any(|command| command.contains(key)));
    }

    #[test]
    fn freebsd_provisioning_recovers_read_only_live_media_tmp() {
        let command =
            freebsd_ssh_provision_commands("ssh-ed25519 AAAAC3NzaFocusedTest agentos-test")
                .join("; ");
        let probe = command
            .find("mkdir -p /tmp/agentos-ssh 2>/dev/null")
            .expect("writable directory probe");
        let mount = command
            .find("mount -t tmpfs tmpfs /tmp")
            .expect("tmpfs fallback");
        let keygen = command.find("ssh-keygen").expect("host key generation");
        assert!(probe < mount && mount < keygen);
    }

    #[test]
    fn provisioning_markers_cannot_match_command_echo() {
        let key = "ssh-ed25519 AAAAC3NzaFocusedTest agentos-test";
        let ubuntu = ubuntu_ssh_provision_commands(key).join("; ");
        let freebsd = freebsd_ssh_provision_commands(key).join("; ");
        for (guest_os, command) in [("ubuntu", ubuntu), ("freebsd", freebsd)] {
            let wrapped = guest_provision_command(&command, guest_os, "ready");
            assert!(!wrapped.contains(&format!("agentos-{guest_os}-ssh-ready")));
            assert!(!wrapped.contains(&format!("agentos-{guest_os}-ssh-failed")));
            assert!(wrapped.contains(&format!("agentos-{guest_os}-ssh-%s\\n' failed")));
        }
    }

    #[test]
    fn live_network_probe_waits_for_post_probe_marker() {
        let command = ubuntu_live_network_probe_command();
        assert!(command.contains("ip link set eth0 up"));
        assert!(command.contains("ping -6 -c1 -W1 ff02::1%eth0"));
        assert!(command.contains("printf 'agentos-live-net-%s\\n' proof"));
        assert!(!command.contains("agentos-live-net-proof"));
    }

    #[test]
    fn ssh_ready_markers_require_key_only_daemon_startup() {
        let key = "ssh-ed25519 AAAAC3NzaFocusedTest agentos-test";
        let ubuntu = ubuntu_ssh_provision_commands(key).join("; ");
        assert!(ubuntu.contains("/cdrom/pool/main/o/openssh/openssh-server_*.deb"));
        assert!(ubuntu.contains("dpkg-deb -x"));
        assert!(ubuntu.contains("ssh-keygen -q -t ed25519"));
        assert!(ubuntu.contains("HostKey=/run/agentos-ssh-host-key"));
        for command in [ubuntu, freebsd_ssh_provision_commands(key).join("; ")] {
            assert!(command.contains("PasswordAuthentication=no"));
            assert!(command.contains("KbdInteractiveAuthentication=no"));
            assert!(command.contains("PubkeyAuthentication=yes"));
            assert!(!command.contains("sshd || true"));
        }
    }

    #[test]
    fn ssh_probes_have_bounded_connection_and_session_liveness() {
        let auth = SSH_AUTH_OPTIONS.join(" ");
        let probe = SSH_PROBE_LIVENESS_OPTIONS.join(" ");
        let session = SSH_SESSION_LIVENESS_OPTIONS.join(" ");
        assert!(auth.contains("BatchMode=yes"));
        assert!(auth.contains("PreferredAuthentications=publickey"));
        assert!(auth.contains("ConnectionAttempts=1"));
        assert!(auth.contains("ConnectTimeout=30"));
        assert!(probe.contains("ServerAliveInterval=5"));
        assert!(probe.contains("ServerAliveCountMax=1"));
        assert!(session.contains("ServerAliveInterval=30"));
        assert!(session.contains("ServerAliveCountMax=20"));
    }

    #[test]
    fn desktop_proof_is_lightweight_and_confined_to_ssh() {
        let script = ubuntu_desktop_provision_script();
        assert!(std::process::Command::new("sh")
            .args(["-n", "-c", script])
            .status()
            .expect("run desktop shell syntax check")
            .success());
        assert!(script.contains("tigervnc-standalone-server_1.15.0+dfsg-2build1_arm64.deb"));
        assert!(script.contains("30e536f05a504ad817b294abee51394143"));
        assert!(script.contains("sha256sum -c"));
        assert!(script.contains("dpkg-deb -x"));
        assert!(script.contains("Xtigervnc :1"));
        assert!(script.contains("-rendernode \"\""));
        assert!(script.contains("gnome-calculator"));
        assert!(!script.contains("apt-get"));
        assert!(script.contains("-localhost yes"));
        assert!(script.contains("-SecurityTypes None"));
        assert!(script.contains("agentos-desktop-local-rfb-ready"));
        assert!(script.contains("/dev/tcp/127.0.0.1/5901"));
        assert_eq!(
            desktop_tunnel_forward_spec(),
            "127.0.0.1:15901:127.0.0.1:5901"
        );
    }

    #[test]
    fn manual_dual_ssh_commands_use_persistent_key_and_distinct_ports() {
        let commands = manual_ssh_commands(Path::new("build/tmp/dual-ssh/id_ed25519"));
        assert!(commands[0].contains("-p 12222"));
        assert!(commands[0].contains("ubuntu@127.0.0.1"));
        assert!(commands[1].contains("-p 12223"));
        assert!(commands[1].contains("root@127.0.0.1"));
        for command in commands {
            assert!(command.contains("build/tmp/dual-ssh/id_ed25519"));
            assert!(command.contains("IdentitiesOnly=yes"));
            assert!(command.contains("StrictHostKeyChecking=no"));
            assert!(command.contains("UserKnownHostsFile=/dev/null"));
        }
    }

    #[test]
    fn freebsd_dynamic_shell_failure_selects_static_rescue() {
        let transcript = "ld-elf.so.1: /lib/libedit.so.8: Unsupported version 0 \
                          of Elf_Verneed entry\nEnter full pathname of shell \
                          or RETURN for /bin/sh:";
        assert!(freebsd_static_rescue_needed(transcript));
        assert!(reject_bad_guest_path("freebsd", transcript).is_ok());
        assert!(!freebsd_static_rescue_needed(
            "Enter full pathname of shell or RETURN for /bin/sh:"
        ));
        assert!(reject_bad_guest_path(
            "freebsd",
            "mountroot>\nManual root filesystem specification:"
        )
        .is_err());
    }

    #[test]
    fn cc_frame_deadline_covers_fault_heavy_guest_round_trip() {
        assert!(CC_FRAME_DEADLINE >= Duration::from_secs(600));
    }

    #[test]
    fn framed_read_preserves_progress_across_socket_timeouts() {
        let (mut reader, mut writer) = UnixStream::pair().expect("create socket pair");
        reader
            .set_read_timeout(Some(Duration::from_millis(10)))
            .expect("set short test timeout");
        let expected: Vec<u8> = (0u8..32u8).collect();
        let send = expected.clone();
        let writer_thread = std::thread::spawn(move || {
            writer.write_all(&send[..7]).expect("write first fragment");
            std::thread::sleep(Duration::from_millis(30));
            writer.write_all(&send[7..]).expect("write second fragment");
        });

        let mut received = [0u8; 32];
        read_cc_frame(&mut reader, &mut received).expect("read fragmented frame");
        writer_thread.join().expect("join writer");
        assert_eq!(received.as_slice(), expected.as_slice());
    }
}
