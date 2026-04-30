use crate::TestArgs;
use anyhow::Context;
use std::io::{Read, Seek, SeekFrom};
use std::os::unix::process::CommandExt;
use std::path::Path;
use std::process::Stdio;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const UBUNTU_DEFAULT_SSH_PORT: u16 = 12222;
const FREEBSD_DEFAULT_SSH_PORT: u16 = 12223;
const UBUNTU_NOCLOUD_PORT: u16 = 18790;

pub fn run(args: &TestArgs) -> anyhow::Result<()> {
    let repo_root = repo_root()?;

    if !args.no_build {
        println!(
            "[xtask:test] Building BOARD={} GUEST_OS={}...",
            args.board, args.guest_os
        );
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

    let log_file = tempfile::NamedTempFile::new().context("failed to create temp log file")?;
    let log_path = log_file.path().to_path_buf();

    let ssh_port = effective_ssh_port(args);
    let _seed_server = if args.guest_os == "ubuntu" {
        Some(start_ubuntu_seed_server(&repo_root)?)
    } else {
        None
    };

    println!("[xtask:test] Launching QEMU for board={}...", args.board);
    let mut qemu =
        spawn_qemu_with_guest(&args.board, &repo_root, &log_path, &args.guest_os, ssh_port)?;

    let result = match args.guest_os.as_str() {
        "ubuntu" => {
            println!(
                "[xtask:test] Waiting for Ubuntu guest SSH on 127.0.0.1:{}...",
                ssh_port
            );
            wait_for_ssh(
                &repo_root,
                ssh_port,
                Duration::from_secs(args.timeout_secs),
                &["systemctl", "is-active", "--quiet", "multi-user.target"],
            )
                .map(|user| {
                    format!(
                        "ssh {}@127.0.0.1:{} systemctl is-active --quiet multi-user.target",
                        user, ssh_port
                    )
                })
        }
        "freebsd" => {
            println!(
                "[xtask:test] Waiting for FreeBSD guest SSH on 127.0.0.1:{}...",
                ssh_port
            );
            wait_for_ssh(
                &repo_root,
                ssh_port,
                Duration::from_secs(args.timeout_secs),
                &[
                    "sh",
                    "-c",
                    "uname -s | grep -qx FreeBSD && service sshd onestatus >/dev/null 2>&1",
                ],
            )
            .map(|user| format!("ssh {}@127.0.0.1:{} FreeBSD sshd onestatus", user, ssh_port))
        }
        _ => {
            /* Success markers: any match is a pass.
             * "agentOS boot complete" = root task + all PDs launched.
             * "[rt] boot complete"    = x86 root-task smoke boot; service PD
             *                           runtime health is tracked separately.
             * "buildroot login:"      = Linux guest reached login prompt (buildroot). */
            let markers: &[&str] = if args.board == "x86_64_generic" {
                &["[rt] boot complete", "agentOS boot complete"]
            } else {
                &["agentOS boot complete", "buildroot login:"]
            };
            wait_for_markers(&log_path, markers, Duration::from_secs(args.timeout_secs))
        }
    };

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
    let dir = tempfile::Builder::new()
        .prefix("agentos-nocloud-ubuntu-")
        .tempdir()
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

fn spawn_qemu_with_guest(
    board: &str,
    repo_root: &Path,
    log_path: &Path,
    guest_os: &str,
    ssh_port: u16,
) -> anyhow::Result<std::process::Child> {
    let log_file = std::fs::File::create(log_path).context("failed to create QEMU log file")?;
    let netdev = qemu_netdev_arg(ssh_port)?;

    let build_image = repo_root.join("build").join(board).join("agentos.img");

    let mut cmd = match board {
        "qemu_virt_aarch64" => {
            let build_dir = repo_root.join("build").join(board);
            let loader = build_dir.join("loader.elf");
            let cc_sock = log_path.with_extension("cc_pd.sock");
            let _ = std::fs::remove_file(&cc_sock);

            let mut c = std::process::Command::new("qemu-system-aarch64");
            c.arg("-machine")
                .arg("virt,virtualization=on,highmem=off,secure=off")
                .arg("-cpu")
                .arg("cortex-a57")
                .arg("-m")
                .arg("2G")
                .arg("-display")
                .arg("none")
                .arg("-monitor")
                .arg("none")
                .arg("-serial")
                .arg("stdio")
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
                .arg("virtserialport,bus=vser0.0,chardev=cc_pd_char,name=cc.0")
                .arg("-device")
                .arg("virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0")
                .arg("-netdev")
                .arg(netdev)
                .arg("-device")
                .arg(format!("loader,file={},cpu-num=0", loader.display()))
                .arg("-device")
                .arg(format!(
                    "loader,file={},addr=0x48000000",
                    build_image.display()
                ));
            if guest_os == "ubuntu" {
                /* ubuntu: root disk on bus.1 (vda). NoCloud data is served
                 * over QEMU user-net; do not also attach a stale seed disk. */
                let ubuntu_img = ubuntu_disk_image(repo_root);
                if ubuntu_img.exists() {
                    println!(
                        "[xtask:test] Ubuntu disk image: {} (snapshot writes)",
                        ubuntu_img.display()
                    );
                    c.args([
                        "-device",
                        "virtio-blk-device,drive=hd0,bus=virtio-mmio-bus.1",
                        "-drive",
                        &format!(
                            "file={},format=raw,id=hd0,if=none,snapshot=on",
                            ubuntu_img.to_str().unwrap()
                        ),
                    ]);
                }
            } else if guest_os == "freebsd" {
                let freebsd_img = freebsd_disk_image(repo_root);
                if freebsd_img.exists() {
                    println!(
                        "[xtask:test] FreeBSD disk image: {}",
                        freebsd_img.display()
                    );
                    c.args([
                        "-device",
                        "virtio-blk-device,drive=hd0,bus=virtio-mmio-bus.31",
                        "-drive",
                        &format!(
                            "file={},format=raw,id=hd0,if=none,snapshot=on",
                            freebsd_img.to_str().unwrap()
                        ),
                    ]);
                }
            } else {
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
            let kernel = repo_root
                .join("microkit-sdk-2.1.0/board/x86_64_generic/release/elf/sel4_32.elf");
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

    let child = cmd
        .stdout(log_file.try_clone()?)
        .stderr(log_file)
        .process_group(0)
        .spawn()
        .context("failed to spawn QEMU")?;
    println!("[xtask:test] QEMU pid={}", child.id());
    Ok(child)
}

fn ubuntu_disk_image(repo_root: &Path) -> std::path::PathBuf {
    if let Ok(home) = std::env::var("HOME") {
        let cached = std::path::PathBuf::from(home)
            .join(".local/agentos-images")
            .join("ubuntu-24.04-aarch64.img");
        if cached.exists() {
            return cached;
        }
    }
    repo_root.join("guest-images/ubuntu-24.04-aarch64.img")
}

fn freebsd_disk_image(repo_root: &Path) -> std::path::PathBuf {
    if let Ok(home) = std::env::var("HOME") {
        let cached = std::path::PathBuf::from(home)
            .join(".local/agentos-images")
            .join("freebsd-14.4-aarch64.img");
        if cached.exists() {
            return cached;
        }
    }
    let linked = repo_root.join("guest-images/freebsd.img");
    if linked.exists() {
        return linked;
    }
    repo_root.join("guest-images/freebsd-14.4-aarch64.img")
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

fn wait_for_ssh(
    repo_root: &Path,
    ssh_port: u16,
    timeout: Duration,
    probe: &[&str],
) -> anyhow::Result<String> {
    if ssh_port == 0 {
        anyhow::bail!("qemu-test requires SSH forwarding; pass --ssh-port <port>");
    }

    let ssh_key = repo_root.join("tests/e2e/id_ed25519");
    if !ssh_key.exists() {
        anyhow::bail!("SSH key not found: {}", ssh_key.display());
    }

    let start = Instant::now();
    let mut last_err = String::new();
    while start.elapsed() < timeout {
        for user in ["root", "ubuntu"] {
            let port_arg = ssh_port.to_string();
            let target = format!("{}@127.0.0.1", user);
            let output = std::process::Command::new("ssh")
                .args([
                    "-i",
                    ssh_key.to_str().unwrap(),
                    "-o",
                    "BatchMode=yes",
                    "-o",
                    "NumberOfPasswordPrompts=0",
                    "-o",
                    "StrictHostKeyChecking=no",
                    "-o",
                    "UserKnownHostsFile=/dev/null",
                    "-o",
                    "ConnectTimeout=1",
                    "-p",
                    &port_arg,
                    &target,
                ])
                .args(probe)
                .output()
                .context("failed to run ssh")?;
            if output.status.success() {
                return Ok(user.to_string());
            }
            last_err = String::from_utf8_lossy(&output.stderr).trim().to_string();
        }
        std::thread::sleep(Duration::from_secs(1));
    }

    if last_err.is_empty() {
        anyhow::bail!("timeout after {}s waiting for guest SSH", timeout.as_secs());
    }
    anyhow::bail!(
        "timeout after {}s waiting for guest SSH: {}",
        timeout.as_secs(),
        last_err
    );
}
