use crate::FaultInjectArgs;
use anyhow::Context;
use std::io::Read;
use std::time::Duration;

pub fn run(args: &FaultInjectArgs) -> anyhow::Result<()> {
    let repo_root = repo_root()?;

    println!(
        "[xtask:fault-inject] Building with FAULT_INJECT=1, BOARD={}...",
        args.board
    );
    crate::cmd_test::run_make(
        &[
            "build",
            "FAULT_INJECT=1",
            &format!("BOARD={}", args.board),
        ],
        &repo_root,
    )
    .context("fault-inject build failed")?;

    let log_file = tempfile::NamedTempFile::new().context("failed to create temp log file")?;
    let log_path = log_file.path().to_path_buf();

    println!(
        "[xtask:fault-inject] Launching QEMU for board={}...",
        args.board
    );
    let mut qemu = spawn_qemu_fault_inject(&args.board, &repo_root, &log_path)?;

    let result = crate::cmd_test::wait_for_markers(
        &log_path,
        &["fault injection test passed", "ALL FAULT INJECT TESTS PASSED"],
        Duration::from_secs(args.timeout_secs),
    );

    let _ = qemu.kill();
    let _ = qemu.wait();

    println!("\n=== Serial output ===");
    if let Ok(mut f) = std::fs::File::open(&log_path) {
        let mut buf = String::new();
        let _ = f.read_to_string(&mut buf);
        print!("{}", buf);
    }
    println!("=====================\n");

    match result {
        Ok(marker) => {
            println!(
                "PASS [board={}]: found marker \"{}\"",
                args.board, marker
            );
            Ok(())
        }
        Err(e) => {
            println!("FAIL [board={}]: {}", args.board, e);
            std::process::exit(1);
        }
    }
}

fn repo_root() -> anyhow::Result<std::path::PathBuf> {
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

fn spawn_qemu_fault_inject(
    board: &str,
    repo_root: &std::path::Path,
    log_path: &std::path::Path,
) -> anyhow::Result<std::process::Child> {
    let log_file =
        std::fs::File::create(log_path).context("failed to create QEMU log file")?;

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
            anyhow::bail!(
                "unknown board: {} — add QEMU invocation to cmd_fault_inject.rs",
                other
            );
        }
    };

    let child = cmd
        .stdout(log_file.try_clone()?)
        .stderr(log_file)
        .spawn()
        .context("failed to spawn QEMU")?;
    Ok(child)
}
