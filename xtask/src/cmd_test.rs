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
    let mut qemu = spawn_qemu(&args.board, &repo_root, &log_path)?;

    let result = wait_for_markers(
        &log_path,
        &["agentOS boot complete", "buildroot login:"],
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

fn spawn_qemu(
    board: &str,
    repo_root: &Path,
    log_path: &Path,
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
            ]);
            c
        }
        "qemu_virt_riscv64" => {
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
                "/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin",
                "-kernel",
                build_image.to_str().unwrap_or("build/qemu_virt_riscv64/agentos.img"),
            ]);
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
        let mut chunk = String::new();
        let bytes_read = file.read_to_string(&mut chunk)?;
        if bytes_read > 0 {
            offset += bytes_read as u64;
            accumulated.push_str(&chunk);

            for &marker in markers {
                if accumulated.contains(marker) {
                    return Ok(marker.to_string());
                }
            }
        }

        std::thread::sleep(Duration::from_millis(200));
    }
}
