use crate::TestArgs;
use anyhow::Context;
use std::io::{Read, Seek, SeekFrom};
use std::os::unix::process::CommandExt;
use std::path::Path;
use std::time::{Duration, Instant};

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

    println!("[xtask:test] Launching QEMU for board={}...", args.board);
    let mut qemu = spawn_qemu_with_guest(&args.board, &repo_root, &log_path, &args.guest_os)?;

    /* Success markers: any match is a pass.
     * "agentOS boot complete" = root task + all PDs launched.
     * "buildroot login:"      = Linux guest reached login prompt (buildroot).
     * "Ubuntu"                = Ubuntu cloud-init banner visible on console. */
    let markers: &[&str] = match args.guest_os.as_str() {
        "ubuntu" => &["Ubuntu 24.04", "ubuntu login:", "login:"],
        _        => &["agentOS boot complete", "buildroot login:"],
    };
    let result = wait_for_markers(
        &log_path,
        markers,
        Duration::from_secs(args.timeout_secs),
    );

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
) -> anyhow::Result<std::process::Child> {
    let log_file = std::fs::File::create(log_path).context("failed to create QEMU log file")?;

    let build_image = repo_root
        .join("build")
        .join(board)
        .join("agentos.img");

    let mut cmd = match board {
        "qemu_virt_aarch64" => {
            let mut c = std::process::Command::new("qemu-system-aarch64");
            c.args([
                "-machine",
                "virt,virtualization=on",
                "-cpu",
                "cortex-a57",
                "-m",
                "2G",
                "-nographic",
                "-kernel",
                build_image.to_str().unwrap_or("build/qemu_virt_aarch64/agentos.img"),
                /* virtio-net (SPI 16 → INTID 48, bus.0) with SSH port forward */
                "-device",  "virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0",
                "-netdev",  "user,id=net0,hostfwd=tcp::2222-:22",
            ]);
            if guest_os == "ubuntu" {
                /* ubuntu: root disk on bus.1 (vda), cloud-init seed on bus.2 (vdb) */
                let ubuntu_img = repo_root.join("guest-images/ubuntu-24.04-aarch64.img");
                let seed_img   = repo_root.join("guest-images/linux-seed.img");
                if ubuntu_img.exists() {
                    c.args([
                        "-device", "virtio-blk-device,drive=hd0,bus=virtio-mmio-bus.1",
                        "-drive",
                        &format!("file={},format=raw,id=hd0,if=none",
                                 ubuntu_img.to_str().unwrap()),
                    ]);
                }
                if seed_img.exists() {
                    c.args([
                        "-device", "virtio-blk-device,drive=seed,bus=virtio-mmio-bus.2",
                        "-drive",
                        &format!("file={},format=raw,id=seed,if=none,readonly=on",
                                 seed_img.to_str().unwrap()),
                    ]);
                }
            } else {
                /* buildroot / default: optional generic disk on bus.1 */
                let disk = repo_root.join("build/qemu_virt_aarch64/disk.img");
                if disk.exists() {
                    c.args([
                        "-device", "virtio-blk-device,drive=hd0,bus=virtio-mmio-bus.1",
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
                "-machine", "virt",
                "-cpu",     "rv64",
                "-m",       "2G",
                "-nographic",
                "-bios",    &bios,
                "-kernel",  build_image.to_str().unwrap_or("build/qemu_virt_riscv64/agentos.img"),
                /* virtio-net (slot 0 → 0x10001000, IRQ 1) with SSH port forward */
                "-device",  "virtio-net-device,netdev=net0",
                "-netdev",  "user,id=net0,hostfwd=tcp::2222-:22",
            ]);
            /* virtio-blk (slot 1 → 0x10002000, IRQ 2) — only if disk image exists */
            let disk = repo_root.join("build/qemu_virt_riscv64/disk.img");
            if disk.exists() {
                c.args([
                    "-device", "virtio-blk-device,drive=hd0",
                    "-drive",
                    &format!(
                        "file={},format=raw,id=hd0,if=none",
                        disk.to_str().unwrap_or("build/qemu_virt_riscv64/disk.img")
                    ),
                ]);
            }
            c
        }
        other => {
            anyhow::bail!("unknown board: {} — add QEMU invocation to cmd_test.rs", other);
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
    if let Ok(output) = std::process::Command::new("brew").args(["--prefix"]).output() {
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
