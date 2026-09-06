use crate::FaultInjectArgs;
use anyhow::{Context, Result};
use std::path::{Path, PathBuf};
use std::time::Duration;

const MSG_CC_FAULT_INJECT: u32 = 0x2612;
const CC_OK: u32 = 0;
const FAULT_NULL_DEREF: u32 = 0x01;
const FAULT_FLAG_VERIFY_RECOVERY: u32 = 0x01;
const FAULT_RESULT_OK: u32 = 0x00;

pub fn run(args: &FaultInjectArgs) -> Result<()> {
    let repo_root = repo_root()?;

    println!(
        "[xtask:fault-inject] Building current-board fault-injection image, BOARD={}...",
        args.board
    );
    crate::cmd_test::run_make(
        &[
            "build",
            "FAULT_INJECT=1",
            "GUEST_OS=none",
            &format!("BOARD={}", args.board),
        ],
        &repo_root,
    )
    .context("fault-inject build failed")?;

    let log_file = tempfile::NamedTempFile::new().context("failed to create temp log file")?;
    let log_path = log_file.path().to_path_buf();
    let cc_sock = log_path.with_extension("cc_pd.sock");

    println!(
        "[xtask:fault-inject] Launching QEMU for board={}...",
        args.board
    );
    let mut qemu = crate::cmd_test::spawn_qemu_with_guest(
        &args.board,
        &repo_root,
        &log_path,
        &cc_sock,
        "none",
        0,
        false,
    )?;

    let result = run_fault_inject_via_cc(&cc_sock, args.timeout_secs, &mut qemu);

    let _ = qemu.kill();
    let _ = qemu.wait();

    let output = std::fs::read_to_string(&log_path).unwrap_or_default();
    println!("\n=== Serial output ===");
    print!("{}", output);
    println!("=====================\n");

    result
}

fn repo_root() -> Result<PathBuf> {
    let output = std::process::Command::new("git")
        .args(["rev-parse", "--show-toplevel"])
        .output()
        .context("failed to run git rev-parse")?;
    anyhow::ensure!(output.status.success(), "not in a git repository");
    let root = String::from_utf8(output.stdout)
        .context("git output is not utf-8")?
        .trim()
        .to_string();
    Ok(PathBuf::from(root))
}

fn run_fault_inject_via_cc(
    cc_sock: &Path,
    timeout_secs: u64,
    qemu: &mut std::process::Child,
) -> Result<()> {
    crate::cmd_test::wait_for_cc_socket(
        cc_sock,
        Duration::from_secs(timeout_secs).min(Duration::from_secs(30)),
        qemu,
    )
    .context("fault-inject CC-PD socket did not become ready")?;

    let reply = crate::cmd_test::cc_call(
        cc_sock,
        MSG_CC_FAULT_INJECT,
        0,
        FAULT_NULL_DEREF,
        FAULT_FLAG_VERIFY_RECOVERY,
        &[],
    )
    .context("MSG_CC_FAULT_INJECT failed")?;

    anyhow::ensure!(
        reply.mr[0] == CC_OK,
        "MSG_CC_FAULT_INJECT returned CC error {}",
        reply.mr[0]
    );
    anyhow::ensure!(
        reply.mr[1] == FAULT_RESULT_OK,
        "fault_inject returned result {}",
        reply.mr[1]
    );

    println!("PASS [fault-inject]: OP_FAULT_INJECT reached fault_inject PD through CC-PD IPC");
    Ok(())
}
