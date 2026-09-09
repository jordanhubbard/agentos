use crate::RunTestsArgs;
use anyhow::{Context, Result};
use std::io::Read;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TapStatus {
    Pass,
    Fail(i32),
}

pub fn run(args: &RunTestsArgs) -> Result<()> {
    if let Some(input_log) = &args.input_log {
        let text = std::fs::read_to_string(input_log)
            .with_context(|| format!("failed to read {}", input_log.display()))?;
        return report_tap_result(&text, &args.board);
    }

    let repo_root = repo_root()?;
    if !args.no_build {
        crate::cmd_test::run_make(
            &["sel4-test-image", &format!("BOARD={}", args.board)],
            &repo_root,
        )
        .context("seL4-target test image build failed")?;
    }

    let log_file = tempfile::NamedTempFile::new().context("failed to create temp log file")?;
    let log_path = log_file.path().to_path_buf();

    let mut qemu = spawn_qemu_test_image(&args.board, &repo_root, &log_path)
        .with_context(|| format!("failed to launch seL4-target TAP image for {}", args.board))?;

    let wait = wait_for_tap_done(&log_path, Duration::from_secs(args.timeout_secs), &mut qemu);

    let _ = qemu.kill();
    let _ = qemu.wait();

    let text = std::fs::read_to_string(&log_path).unwrap_or_default();
    println!("\n=== Serial output ===");
    print!("{}", text);
    println!("=====================\n");

    wait?;
    report_tap_result(&text, &args.board)
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

fn spawn_qemu_test_image(
    board: &str,
    repo_root: &Path,
    log_path: &Path,
) -> Result<std::process::Child> {
    let log_file = std::fs::File::create(log_path).context("failed to create QEMU log file")?;
    let build_dir = repo_root.join("build").join(format!("{board}-test"));

    let mut cmd = match board {
        "qemu_virt_aarch64" => {
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
                .arg("-device")
                .arg(format!(
                    "loader,file={},cpu-num=0",
                    build_dir.join("loader.elf").display()
                ))
                .arg("-device")
                .arg(format!(
                    "loader,file={},addr=0x48000000",
                    build_dir.join("agentos.img").display()
                ));
            c
        }
        "x86_64_generic" => {
            let kernel = crate::cmd_test::sel4_sdk_path()?
                .join("board/x86_64_generic/release/elf/sel4_32.elf");
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
                .arg(build_dir.join("root_task.elf"));
            c
        }
        other => anyhow::bail!(
            "unknown board: {} -- add QEMU invocation to cmd_run_tests.rs",
            other
        ),
    };

    let child = cmd
        .stdout(log_file.try_clone()?)
        .stderr(log_file)
        .process_group(0)
        .spawn()
        .context("failed to spawn QEMU")?;
    println!("[xtask:run-tests] QEMU pid={}", child.id());
    Ok(child)
}

fn wait_for_tap_done(
    log_path: &Path,
    timeout: Duration,
    qemu: &mut std::process::Child,
) -> Result<()> {
    let start = Instant::now();
    let mut buf = String::new();

    loop {
        if start.elapsed() >= timeout {
            anyhow::bail!("timeout after {}s waiting for TAP_DONE", timeout.as_secs());
        }
        if let Some(status) = qemu.try_wait().context("failed to poll QEMU")? {
            anyhow::bail!("QEMU exited with status {status} before TAP_DONE");
        }

        buf.clear();
        if let Ok(mut f) = std::fs::File::open(log_path) {
            let _ = f.read_to_string(&mut buf);
            if parse_tap_done(&buf).is_some() {
                return Ok(());
            }
        }
        std::thread::sleep(Duration::from_millis(200));
    }
}

pub fn parse_tap_done(output: &str) -> Option<TapStatus> {
    for line in output.lines() {
        let Some(rest) = line.trim().strip_prefix("TAP_DONE:") else {
            continue;
        };
        let code = rest.trim().parse::<i32>().ok()?;
        return Some(if code == 0 {
            TapStatus::Pass
        } else {
            TapStatus::Fail(code)
        });
    }
    None
}

fn report_tap_result(output: &str, board: &str) -> Result<()> {
    match parse_tap_done(output) {
        Some(TapStatus::Pass) => {
            println!("PASS [board={}]: TAP_DONE:0", board);
            Ok(())
        }
        Some(TapStatus::Fail(code)) => {
            anyhow::bail!("FAIL [board={}]: TAP_DONE:{}", board, code)
        }
        None => anyhow::bail!("FAIL [board={}]: no TAP_DONE sentinel found", board),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parser_accepts_success_sentinel() {
        assert_eq!(
            parse_tap_done("TAP version 14\nok 1 - boot\n1..1\nTAP_DONE:0\n"),
            Some(TapStatus::Pass)
        );
    }

    #[test]
    fn parser_reports_failure_code() {
        assert_eq!(
            parse_tap_done("not ok 1 - boot\nTAP_DONE:2\n"),
            Some(TapStatus::Fail(2))
        );
    }

    #[test]
    fn parser_reports_missing_sentinel_as_incomplete() {
        assert_eq!(
            parse_tap_done("TAP version 14\nok 1 - still running\n"),
            None
        );
    }
}
