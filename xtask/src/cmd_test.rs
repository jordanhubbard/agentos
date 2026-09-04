use crate::TestArgs;
use anyhow::Context;
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::unix::net::UnixStream;
use std::os::unix::process::CommandExt;
use std::path::Path;
use std::process::{Child, Stdio};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const UBUNTU_DEFAULT_SSH_PORT: u16 = 12222;
const FREEBSD_DEFAULT_SSH_PORT: u16 = 12223;
const UBUNTU_NOCLOUD_PORT: u16 = 18790;
const CC_WIRE_SHMEM_SIZE: usize = 4096;
const CC_REQ_SIZE: usize = 4 + 12 + CC_WIRE_SHMEM_SIZE;
const CC_REPLY_SIZE: usize = 16 + CC_WIRE_SHMEM_SIZE;
const CC_IO_TIMEOUT: Duration = Duration::from_secs(45);
const CC_OK: u32 = 0;
const CC_ERR_RELAY_FAULT: u32 = 8;
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

pub fn run(args: &TestArgs) -> anyhow::Result<()> {
    let repo_root = repo_root()?;

    if args.assert_emulated_net
        || args.assert_emulated_blk
        || args.assert_emulated_console
        || args.assert_agentos_virtio
        || args.assert_ubuntu_live
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
    if args.assert_emulated_console || args.assert_agentos_virtio || args.assert_ubuntu_live {
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
        if args.assert_ubuntu_live {
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

    let needs_ssh_probe = !matches!(args.guest_os.as_str(), "ubuntu" | "freebsd");
    let ssh_port = if needs_ssh_probe {
        effective_ssh_port(args)
    } else {
        0
    };
    let _seed_server = if args.guest_os == "ubuntu" && needs_ssh_probe {
        Some(start_ubuntu_seed_server(&repo_root)?)
    } else {
        None
    };

    println!("[xtask:test] Launching QEMU for board={}...", args.board);
    let cc_sock = log_path.with_extension("cc_pd.sock");
    let mut qemu = spawn_qemu_with_guest(
        &args.board,
        &repo_root,
        &log_path,
        &cc_sock,
        &args.guest_os,
        ssh_port,
        args.assert_ubuntu_live,
    )?;

    let mut result = if args.assert_emulated_net {
        println!(
            "[xtask:test] Waiting for emulated virtio-net guest proof in {}...",
            log_path.display()
        );
        wait_for_emulated_net(&log_path, Duration::from_secs(args.timeout_secs))
    } else if args.assert_emulated_blk {
        println!(
            "[xtask:test] Waiting for emulated virtio-blk guest proof in {}...",
            log_path.display()
        );
        wait_for_emulated_blk(&log_path, Duration::from_secs(args.timeout_secs))
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
                if args.assert_ubuntu_live {
                    "ubuntu-live"
                } else {
                    "ubuntu"
                },
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

    if result.is_ok()
        && (args.assert_emulated_console
            || args.assert_agentos_virtio
            || args.assert_ubuntu_live)
    {
        let mut required = vec![
            "emulated virtio-console: guest probed",
            "emulated virtio-console: guest DRIVER_OK",
            "emulated virtio-console: pumped ",
            "emulated virtio-console: pumped input serial_virt->guest",
        ];
        if args.assert_agentos_virtio || args.assert_ubuntu_live {
            required.extend_from_slice(&[
                "emulated virtio-net: guest probed",
                "emulated virtio-net: guest DRIVER_OK",
                "emulated virtio-net: pumped",
                "emulated virtio-blk: guest probed",
                "emulated virtio-blk: guest DRIVER_OK",
                "emulated virtio-blk: pumped",
                    "emulated virtio-blk: agentOS host media ready",
                    "emulated virtio-blk: host-media read",
            ]);
        }
        let console = wait_for_all_markers(
            &log_path,
            &required,
            Duration::from_secs(10),
        );
        result = match (result, console) {
            (Ok(login), Ok(_)) if args.assert_ubuntu_live => Ok(format!(
                "{login}; full Ubuntu Casper userspace uses agentOS virtio net + blk + console"
            )),
            (Ok(login), Ok(_)) if args.assert_agentos_virtio => Ok(format!(
                "{login}; agentOS virtio net + blk + console probed, DRIVER_OK, and pumped real I/O"
            )),
            (Ok(login), Ok(_)) => Ok(format!(
                "{login}; emulated virtio-console probed + DRIVER_OK + bidirectional I/O"
            )),
            (_, Err(err)) => Err(err.context(
                "Ubuntu login succeeded but virtio-console proof was incomplete",
            )),
            (Err(err), _) => Err(err),
        };
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
            println!("FAIL [board={}]: {}", args.board, e);
            anyhow::bail!("test failed for board {}: {}", args.board, e);
        }
    }
}

fn effective_ssh_port(args: &TestArgs) -> u16 {
    if args.guest_os == "ubuntu" && args.ssh_port == 0 {
        UBUNTU_DEFAULT_SSH_PORT
    } else if args.guest_os == "freebsd" && args.ssh_port == 0 {
        FREEBSD_DEFAULT_SSH_PORT
    } else {
        args.ssh_port
    }
}

struct SeedServer {
    child: std::process::Child,
    _dir: tempfile::TempDir,
}

impl Drop for SeedServer {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn start_ubuntu_seed_server(repo_root: &Path) -> anyhow::Result<SeedServer> {
    let pubkey_path = repo_root.join("tests/e2e/id_ed25519.pub");
    let pubkey_raw = std::fs::read_to_string(&pubkey_path)
        .with_context(|| format!("failed to read {}", pubkey_path.display()))?;
    let pubkey = pubkey_raw.trim();
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .context("system clock is before UNIX_EPOCH")?
        .as_secs();
    let meta_data = format!(
        "instance-id: agentos-linux-ubuntu-{}-{}\nlocal-hostname: agentos-linux\n",
        std::process::id(),
        now
    );
    let user_data = format!(
        r#"#cloud-config
disable_root: false
ssh_pwauth: true
ssh_authorized_keys:
  - {pubkey}
users:
  - default
  - name: ubuntu
    lock_passwd: false
    groups: [adm, sudo]
    shell: /bin/bash
    ssh_authorized_keys:
      - {pubkey}
  - name: root
    lock_passwd: false
    ssh_authorized_keys:
      - {pubkey}
chpasswd:
  expire: false
  users:
    - name: root
      password: agentos
      type: text
write_files:
  - path: /root/.ssh/authorized_keys
    owner: root:root
    permissions: '0600'
    content: |
      {pubkey}
"#,
        pubkey = pubkey
    );

    ensure_host_port_available(UBUNTU_NOCLOUD_PORT)?;
    let tmp_dir = repo_root.join("build/tmp");
    std::fs::create_dir_all(&tmp_dir)
        .with_context(|| format!("failed to create {}", tmp_dir.display()))?;
    let dir = tempfile::Builder::new()
        .prefix("agentos-nocloud-ubuntu-")
        .tempdir_in(&tmp_dir)
        .context("failed to create Ubuntu NoCloud tempdir")?;
    std::fs::write(dir.path().join("meta-data"), meta_data)
        .context("failed to write NoCloud meta-data")?;
    std::fs::write(dir.path().join("user-data"), user_data)
        .context("failed to write NoCloud user-data")?;
    std::fs::write(dir.path().join("vendor-data"), "")
        .context("failed to write NoCloud vendor-data")?;

    let mut child = std::process::Command::new("python3")
        .args([
            "-m",
            "http.server",
            &UBUNTU_NOCLOUD_PORT.to_string(),
            "--bind",
            "127.0.0.1",
            "--directory",
            dir.path().to_str().unwrap(),
        ])
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit())
        .spawn()
        .context("failed to start python3 NoCloud seed server")?;
    std::thread::sleep(Duration::from_secs(1));
    if let Some(status) = child
        .try_wait()
        .context("failed to poll NoCloud seed server")?
    {
        anyhow::bail!("Ubuntu NoCloud seed server exited early: {}", status);
    }

    println!(
        "[xtask:test] Ubuntu NoCloud-Net seed server: http://127.0.0.1:{}/",
        UBUNTU_NOCLOUD_PORT
    );

    Ok(SeedServer { child, _dir: dir })
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

pub fn spawn_qemu_with_guest(
    board: &str,
    repo_root: &Path,
    log_path: &Path,
    cc_sock: &Path,
    guest_os: &str,
    ssh_port: u16,
    ubuntu_live: bool,
) -> anyhow::Result<std::process::Child> {
    let log_file = std::fs::File::create(log_path).context("failed to create QEMU log file")?;
    let netdev = qemu_netdev_arg(ssh_port)?;

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
            if guest_os != "ubuntu" {
                c.args([
                    "-device",
                    "virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0",
                    "-netdev",
                    &netdev,
                ]);
            }
            if guest_os == "ubuntu" || guest_os == "both" {
                /*
                 * The single Ubuntu guest gets a host block device on bus.8,
                 * owned only by agentOS virtio_blk. Its guest DTB still
                 * advertises only the emulated device at 0x0a020000. Dual
                 * guest mode retains the bus.1 guest-passthrough crutch.
                 */
                let ubuntu_img = ubuntu_disk_image(repo_root);
                if guest_os == "ubuntu" && ubuntu_img.exists() {
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
                } else if guest_os == "both" && ubuntu_img.exists() {
                    println!(
                        "[xtask:test] Ubuntu disk image: {} (snapshot writes)",
                        ubuntu_img.display()
                    );
                    c.args([
                        "-device",
                        "virtio-blk-device,drive=hd0,bus=virtio-mmio-bus.1",
                        "-drive",
                        &format!(
                            "file={},format=raw,id=hd0,if=none,readonly=on,file.locking=off",
                            ubuntu_img.to_str().unwrap()
                        ),
                    ]);
                }
            }
            if guest_os == "freebsd" || guest_os == "both" {
                let freebsd_img = freebsd_disk_image(repo_root);
                if freebsd_img.exists() {
                    println!("[xtask:test] FreeBSD disk image: {}", freebsd_img.display());
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
            if guest_os != "ubuntu" && guest_os != "freebsd" && guest_os != "both" {
                /* buildroot / default: optional generic disk on bus.1 */
                let disk = build_dir.join("disk.img");
                if disk.exists() {
                    c.args([
                        "-device",
                        "virtio-blk-device,drive=hd0,bus=virtio-mmio-bus.1",
                        "-drive",
                        &format!(
                            "file={},format=raw,id=hd0,if=none",
                            disk.to_str().unwrap_or("build/qemu_virt_aarch64/disk.img")
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
            let kernel =
                repo_root.join("microkit-sdk-2.1.0/board/x86_64_generic/release/elf/sel4_32.elf");
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

fn qemu_netdev_arg(ssh_port: u16) -> anyhow::Result<String> {
    if ssh_port == 0 {
        return Ok("user,id=net0".to_string());
    }
    ensure_host_port_available(ssh_port)?;
    Ok(format!(
        "user,id=net0,hostfwd=tcp:127.0.0.1:{}-:22",
        ssh_port
    ))
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
) -> anyhow::Result<String> {
    let start = Instant::now();
    let mut file = std::fs::File::open(log_path).context("failed to open log file")?;
    let mut offset: u64 = 0;
    let mut accumulated = String::new();

    loop {
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
];

const EMU_BLK_GUEST_ANY: &[&str] = &[
    "0a020000.virtio_mmio",
    "a020000.virtio_mmio",
    "virtio_blk",
    "[vda]",
];

fn wait_for_emulated_blk(log_path: &Path, timeout: Duration) -> anyhow::Result<String> {
    let start = Instant::now();
    let mut file = std::fs::File::open(log_path).context("failed to open log file")?;
    let mut offset: u64 = 0;
    let mut accumulated = String::new();

    loop {
        if start.elapsed() >= timeout {
            let missing: Vec<&str> = EMU_BLK_REQUIRED
                .iter()
                .copied()
                .filter(|m| !accumulated.contains(m))
                .collect();
            let guest_ok = EMU_BLK_GUEST_ANY
                .iter()
                .any(|m| accumulated.contains(m));
            anyhow::bail!(
                "emulated virtio-blk proof timeout after {}s; missing VMM markers {:?}; guest IPA/disk observed={}",
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

            let vmm_ok = EMU_BLK_REQUIRED
                .iter()
                .all(|m| accumulated.contains(m));
            let guest_ok = EMU_BLK_GUEST_ANY
                .iter()
                .any(|m| accumulated.contains(m));
            if vmm_ok && guest_ok {
                return Ok("emulated virtio-blk: guest probed + DRIVER_OK + pumped + guest IPA/disk".to_string());
            }
        }

        std::thread::sleep(Duration::from_millis(200));
    }
}

fn wait_for_emulated_net(log_path: &Path, timeout: Duration) -> anyhow::Result<String> {
    let start = Instant::now();
    let mut file = std::fs::File::open(log_path).context("failed to open log file")?;
    let mut offset: u64 = 0;
    let mut accumulated = String::new();

    loop {
        if start.elapsed() >= timeout {
            let missing: Vec<&str> = EMU_NET_REQUIRED
                .iter()
                .copied()
                .filter(|m| !accumulated.contains(m))
                .collect();
            let guest_ok = EMU_NET_GUEST_ANY
                .iter()
                .any(|m| accumulated.contains(m));
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

            let vmm_ok = EMU_NET_REQUIRED
                .iter()
                .all(|m| accumulated.contains(m));
            let guest_ok = EMU_NET_GUEST_ANY
                .iter()
                .any(|m| accumulated.contains(m));
            if vmm_ok && guest_ok {
                return Ok("emulated virtio-net: guest probed + DRIVER_OK + pumped + guest IPA/MAC".to_string());
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
    stream: UnixStream,
}

impl CcClient {
    fn connect(cc_sock: &Path) -> anyhow::Result<Self> {
        let stream = UnixStream::connect(cc_sock)
            .with_context(|| format!("failed to connect to {}", cc_sock.display()))?;
        stream
            .set_read_timeout(Some(CC_IO_TIMEOUT))
            .context("failed to set CC socket read timeout")?;
        stream
            .set_write_timeout(Some(CC_IO_TIMEOUT))
            .context("failed to set CC socket write timeout")?;
        Ok(Self { stream })
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

        self.stream
            .write_all(&req)
            .context("failed to write CC frame")?;

        let mut raw = [0u8; CC_REPLY_SIZE];
        self.stream
            .read_exact(&mut raw)
            .context("failed to read CC frame")?;
        Ok(CcReply {
            mr: [rd32(&raw, 0), rd32(&raw, 4), rd32(&raw, 8), rd32(&raw, 12)],
            shmem: raw[16..].to_vec(),
        })
    }
}

fn wait_for_guest_console_login_via_cc(
    cc_sock: &Path,
    guest_handle: u32,
    guest_os: &str,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    let mut cc = connect_cc_client(cc_sock, timeout.min(Duration::from_secs(30)), qemu)?;

    let start = Instant::now();
    let mut transcript = String::new();
    let mut matched_prompt = None;
    let prompt_markers = guest_prompt_markers(guest_os);
    let mut freebsd_console_type_accepted = false;
    let mut freebsd_installer_shell_requested = false;

    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "waiting for guest login prompt via CC-PD API")?;
        match cc_log_stream_for_handle(&mut cc, guest_handle) {
            Ok(chunk) => {
                if !chunk.is_empty() {
                    transcript.push_str(&chunk);
                    reject_bad_guest_path(guest_os, &transcript)?;
                    if guest_os == "freebsd"
                        && !freebsd_console_type_accepted
                        && transcript.contains("Console type [vt100]:")
                    {
                        println!(
                            "[xtask:test] FreeBSD console type prompt reached; accepting vt100"
                        );
                        cc_send_raw_byte(&mut cc, guest_handle, b'\r')?;
                        freebsd_console_type_accepted = true;
                    }
                    if guest_os == "freebsd"
                        && !freebsd_installer_shell_requested
                        && transcript.contains("FreeBSD Installer")
                        && transcript.contains("begin an installation or use the live")
                    {
                        println!("[xtask:test] FreeBSD installer menu reached; selecting shell");
                        cc_send_raw_bytes(&mut cc, guest_handle, b"\t\r")?;
                        freebsd_installer_shell_requested = true;
                    }

                    let matched = if let Some(marker) = prompt_markers
                        .iter()
                        .find(|marker| transcript.contains(**marker))
                    {
                        if guest_os == "ubuntu-live"
                            && !transcript.contains("Ubuntu 26.04")
                        {
                            None
                        } else {
                            Some((*marker).to_string())
                        }
                    } else if guest_os == "freebsd"
                        && freebsd_installer_shell_requested
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
                println!("[xtask:test] CC console drain not ready yet: {err:#}");
                if !is_transient_cc_read_error(&err) {
                    if let Ok(next) = CcClient::connect(cc_sock) {
                        cc = next;
                    }
                }
            }
        }
        std::thread::sleep(Duration::from_secs(1));
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
        &mut cc,
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
                && transcript.contains("failed with error")
            || transcript.contains("Enter full pathname")
            || transcript.contains("single-user"))
    {
        anyhow::bail!(
            "FreeBSD reached mountroot, maintenance, or single-user fallback instead of a normal login or configured installer shell; tail:\n{}",
            tail_chars(transcript, 4000)
        );
    }
    Ok(())
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
        return verify_ubuntu_live_console_and_net(
            cc_sock,
            cc,
            guest_handle,
            timeout,
            qemu,
        );
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
        let chunk = match cc_log_stream_for_handle(cc, guest_handle) {
            Ok(chunk) => chunk,
            Err(err) => {
                println!("[xtask:test] CC console post-input drain not ready yet: {err:#}");
                if !is_transient_cc_read_error(&err) {
                    if let Ok(next) = CcClient::connect(cc_sock) {
                        *cc = next;
                    }
                }
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

fn verify_ubuntu_live_console_and_net(
    cc_sock: &Path,
    cc: &mut CcClient,
    guest_handle: u32,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    cc_send_raw_bytes(cc, guest_handle, b"ubuntu\n")?;

    let phase_timeout = timeout.min(Duration::from_secs(180));
    let login_start = Instant::now();
    let mut transcript = String::new();
    let mut blank_password_sent = false;
    let mut login_attempts = 1u32;
    while login_start.elapsed() < phase_timeout {
        ensure_qemu_running(qemu, "logging into Ubuntu live console")?;
        let chunk = match cc_log_stream_for_handle(cc, guest_handle) {
            Ok(chunk) => chunk,
            Err(err) => {
                println!("[xtask:test] CC live-login drain not ready yet: {err:#}");
                if !is_transient_cc_read_error(&err) {
                    if let Ok(next) = CcClient::connect(cc_sock) {
                        *cc = next;
                    }
                }
                String::new()
            }
        };
        if !chunk.is_empty() {
            transcript.push_str(&chunk);
            if transcript.contains("Login incorrect") {
                if transcript.contains("pam_nologin")
                    || transcript.contains("System is booting up")
                {
                    anyhow::ensure!(
                        login_attempts < 5,
                        "Ubuntu live user sessions never became available; tail:\n{}",
                        tail_chars(&transcript, 2000)
                    );
                    std::thread::sleep(Duration::from_secs(5));
                    cc_send_raw_bytes(cc, guest_handle, b"ubuntu\n")?;
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
                cc_send_raw_byte(cc, guest_handle, b'\n')?;
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
    cc_send_raw_bytes(
        cc,
        guest_handle,
        b"printf 'agentos-live-%s\\n' proof\r",
    )?;
    let proof_start = Instant::now();
    let mut output = String::new();
    while proof_start.elapsed() < phase_timeout {
        ensure_qemu_running(qemu, "waiting for Ubuntu live userspace proof")?;
        let chunk = cc_log_stream_for_handle(cc, guest_handle).unwrap_or_default();
        if !chunk.is_empty() {
            output.push_str(&chunk);
            if output.contains("agentos-live-proof") {
                /*
                 * Link-up emits IPv6 control traffic; the bounded all-nodes
                 * ping guarantees a guest-originated frame without DHCP.
                 */
                cc_send_raw_bytes(
                    cc,
                    guest_handle,
                    b"sudo -n ip link set eth0 up\r",
                )?;
                std::thread::sleep(Duration::from_secs(1));
                cc_send_raw_bytes(
                    cc,
                    guest_handle,
                    b"ping -6 -c1 -W1 ff02::1%eth0\r",
                )?;
                return Ok(String::from(
                    "logged into Ubuntu live userspace, executed a command, and emitted a network probe",
                ));
            }
        }
        std::thread::sleep(Duration::from_millis(500));
    }

    anyhow::bail!(
        "Ubuntu live shell did not complete the userspace/network proof; login tail:\n{}\ncommand tail:\n{}",
        tail_chars(&transcript, 2000),
        tail_chars(&output, 2000)
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
    cc_sock: &Path,
    os_type: u8,
    ram_mb: u32,
    label: &str,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<u32> {
    let start = Instant::now();
    let mut cc = connect_cc_client(cc_sock, timeout.min(Duration::from_secs(30)), qemu)?;
    let mut attempts = 0u32;
    let mut last_err = String::from("no create attempt completed");

    while start.elapsed() < timeout {
        attempts += 1;
        ensure_qemu_running(qemu, &format!("creating {label} guest through CC-PD"))?;

        match try_create_guest_via_cc(&mut cc, os_type, ram_mb) {
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
                if attempts == 1 || attempts % 5 == 0 {
                    println!(
                        "[xtask:test] {label} guest create transport not ready yet: {last_err}"
                    );
                }
                if let Ok(next) = CcClient::connect(cc_sock) {
                    cc = next;
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

fn wait_for_dual_guest_consoles_via_cc(
    cc_sock: &Path,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    let start = Instant::now();
    let create_timeout = timeout.min(Duration::from_secs(120));

    let freebsd_handle = create_guest_via_cc_wait(
        cc_sock,
        VIBEOS_TYPE_FREEBSD,
        512,
        "FreeBSD",
        create_timeout,
        qemu,
    )
    .context("failed to create FreeBSD guest through vm_manager")?;

    let freebsd = wait_for_guest_console_login_via_cc(
        cc_sock,
        freebsd_handle,
        "freebsd",
        timeout.saturating_sub(start.elapsed()),
        qemu,
    )?;

    let linux_handle = create_guest_via_cc_wait(
        cc_sock,
        VIBEOS_TYPE_LINUX,
        512,
        "Linux",
        create_timeout,
        qemu,
    )
    .context("failed to create Linux guest through vm_manager")?;

    let linux = wait_for_guest_console_login_via_cc(
        cc_sock,
        linux_handle,
        "ubuntu",
        timeout.saturating_sub(start.elapsed()),
        qemu,
    )?;

    let mut cc = connect_cc_client(cc_sock, timeout.min(Duration::from_secs(30)), qemu)?;
    let linux_after_freebsd = verify_guest_console_input(
        cc_sock,
        &mut cc,
        linux_handle,
        "ubuntu",
        Duration::from_secs(20),
        qemu,
    )?;
    let freebsd_after_linux = verify_guest_console_input(
        cc_sock,
        &mut cc,
        freebsd_handle,
        "freebsd",
        Duration::from_secs(20),
        qemu,
    )?;
    let linux_suspend_state = suspend_guest_via_cc(&mut cc, linux_handle)
        .context("failed to suspend Linux guest after dual console proof")?;
    println!(
        "[xtask:test] suspended Linux guest handle={linux_handle} state={linux_suspend_state}"
    );
    let linux_resume_state = resume_guest_via_cc(&mut cc, linux_handle)
        .context("failed to resume Linux guest after dual console proof")?;
    println!("[xtask:test] resumed Linux guest handle={linux_handle} state={linux_resume_state}");
    let linux_after_resume = verify_guest_console_input(
        cc_sock,
        &mut cc,
        linux_handle,
        "ubuntu",
        Duration::from_secs(20),
        qemu,
    )?;

    destroy_guest_via_cc(&mut cc, linux_handle).context("failed to destroy Linux guest")?;
    destroy_guest_via_cc(&mut cc, freebsd_handle).context("failed to destroy FreeBSD guest")?;

    Ok(format!(
        "dual CC/vm_manager consoles ready: linux_handle={linux_handle} ({linux}; after_freebsd={linux_after_freebsd}; after_resume={linux_after_resume}); freebsd_handle={freebsd_handle} ({freebsd}; after_linux={freebsd_after_linux}); both destroyed"
    ))
}

fn is_transient_cc_read_error(err: &anyhow::Error) -> bool {
    let rendered = format!("{err:#}");
    rendered.contains("Resource temporarily unavailable")
        || rendered.contains("timed out")
        || rendered.contains("WouldBlock")
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

fn cc_log_stream_for_handle(cc: &mut CcClient, guest_handle: u32) -> anyhow::Result<String> {
    let reply = cc
        .call(MSG_CC_LOG_STREAM, guest_handle, 0, 0, &[])
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

    let reply = cc
        .call(MSG_CC_SEND_INPUT, guest_handle, 0, 0, &shmem)
        .context("MSG_CC_SEND_INPUT failed")?;
    anyhow::ensure!(
        reply.mr[0] == CC_OK,
        "MSG_CC_SEND_INPUT returned ok={}",
        reply.mr[0]
    );
    Ok(())
}

fn cc_send_raw_bytes(cc: &mut CcClient, guest_handle: u32, bytes: &[u8]) -> anyhow::Result<()> {
    for byte in bytes {
        cc_send_raw_byte(cc, guest_handle, *byte)?;
    }
    Ok(())
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
