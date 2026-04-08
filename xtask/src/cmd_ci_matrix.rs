use crate::CiMatrixArgs;
use std::time::Duration;

pub struct TestCase {
    pub name: &'static str,
    pub board: &'static str,
    pub guest_os: &'static str,
    pub extra_make_args: &'static [&'static str],
    pub success_marker: &'static str,
}

pub const TEST_MATRIX: &[TestCase] = &[
    TestCase {
        name: "buildroot-qemu",
        board: "qemu_virt_aarch64",
        guest_os: "buildroot",
        extra_make_args: &[],
        success_marker: "buildroot login:",
    },
    TestCase {
        name: "linux-qemu",
        board: "qemu_virt_aarch64",
        guest_os: "linux",
        extra_make_args: &[],
        success_marker: "agentOS boot complete",
    },
];

pub fn run(args: &CiMatrixArgs) -> anyhow::Result<()> {
    if args.list_only {
        println!("CI test matrix:");
        println!("{:<25} {:<20} {:<15}", "NAME", "BOARD", "GUEST_OS");
        println!("{}", "-".repeat(65));
        for tc in TEST_MATRIX {
            if matches_filter(tc, &args.filter) {
                println!("{:<25} {:<20} {:<15}", tc.name, tc.board, tc.guest_os);
            }
        }
        return Ok(());
    }

    let repo_root = repo_root()?;
    let mut pass = 0usize;
    let mut fail = 0usize;
    let mut failures: Vec<String> = Vec::new();

    for tc in TEST_MATRIX {
        if !matches_filter(tc, &args.filter) {
            continue;
        }

        println!("\n[ci-matrix] Running: {}", tc.name);
        println!(
            "[ci-matrix]   board={} guest_os={} marker=\"{}\"",
            tc.board, tc.guest_os, tc.success_marker
        );

        // Build — collect owned strings first so &str slices remain valid
        let board_arg = format!("BOARD={}", tc.board);
        let guest_arg = format!("GUEST_OS={}", tc.guest_os);
        let mut make_args_owned: Vec<String> =
            vec!["build".to_string(), board_arg, guest_arg];
        for &extra in tc.extra_make_args {
            make_args_owned.push(extra.to_string());
        }
        let make_args_refs: Vec<&str> = make_args_owned.iter().map(|s| s.as_str()).collect();
        if let Err(e) = crate::cmd_test::run_make(&make_args_refs, &repo_root) {
            println!("[ci-matrix] FAIL ({}): build error: {}", tc.name, e);
            fail += 1;
            failures.push(format!("{}: build failed: {}", tc.name, e));
            continue;
        }

        // Spawn QEMU and check for success marker
        let log_file = match tempfile::NamedTempFile::new() {
            Ok(f) => f,
            Err(e) => {
                println!("[ci-matrix] FAIL ({}): could not create temp file: {}", tc.name, e);
                fail += 1;
                failures.push(format!("{}: temp file error: {}", tc.name, e));
                continue;
            }
        };
        let log_path = log_file.path().to_path_buf();

        let qemu_result = spawn_qemu_for_board(tc.board, &repo_root, &log_path);
        let mut qemu = match qemu_result {
            Ok(q) => q,
            Err(e) => {
                println!("[ci-matrix] FAIL ({}): QEMU spawn error: {}", tc.name, e);
                fail += 1;
                failures.push(format!("{}: QEMU spawn failed: {}", tc.name, e));
                continue;
            }
        };

        let result = crate::cmd_test::wait_for_markers(
            &log_path,
            &[tc.success_marker],
            Duration::from_secs(120),
        );

        let _ = qemu.kill();
        let _ = qemu.wait();

        match result {
            Ok(_) => {
                println!("[ci-matrix] PASS: {}", tc.name);
                pass += 1;
            }
            Err(e) => {
                println!("[ci-matrix] FAIL ({}): {}", tc.name, e);
                fail += 1;
                failures.push(format!("{}: {}", tc.name, e));
            }
        }
    }

    println!("\n=== CI Matrix Results ===");
    println!("PASS: {}  FAIL: {}", pass, fail);
    if !failures.is_empty() {
        println!("\nFailed tests:");
        for f in &failures {
            println!("  - {}", f);
        }
        std::process::exit(1);
    }
    Ok(())
}

fn matches_filter(tc: &TestCase, filter: &Option<String>) -> bool {
    match filter {
        None => true,
        Some(f) => tc.name.contains(f.as_str()),
    }
}

fn repo_root() -> anyhow::Result<std::path::PathBuf> {
    let output = std::process::Command::new("git")
        .args(["rev-parse", "--show-toplevel"])
        .output()
        .map_err(|e| anyhow::anyhow!("failed to run git rev-parse: {}", e))?;
    anyhow::ensure!(output.status.success(), "not in a git repository");
    let root = String::from_utf8(output.stdout)
        .map_err(|e| anyhow::anyhow!("git output is not utf-8: {}", e))?
        .trim()
        .to_string();
    Ok(std::path::PathBuf::from(root))
}

fn spawn_qemu_for_board(
    board: &str,
    repo_root: &std::path::Path,
    log_path: &std::path::Path,
) -> anyhow::Result<std::process::Child> {
    let log_file = std::fs::File::create(log_path)
        .map_err(|e| anyhow::anyhow!("failed to create QEMU log file: {}", e))?;

    let build_image = repo_root.join("build").join(board).join("agentos.img");
    let image_str = build_image
        .to_str()
        .unwrap_or("build/qemu_virt_aarch64/agentos.img")
        .to_string();

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
                &image_str,
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
                &image_str,
            ]);
            c
        }
        other => {
            anyhow::bail!("unknown board: {}", other);
        }
    };

    let child = cmd
        .stdout(log_file.try_clone()?)
        .stderr(log_file)
        .spawn()
        .map_err(|e| anyhow::anyhow!("failed to spawn QEMU: {}", e))?;
    Ok(child)
}
