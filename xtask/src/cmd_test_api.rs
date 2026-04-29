//! `cargo xtask test-api` — compile and run the agentOS API test suite.
//!
//! Each test file in `tests/api/` is compiled with `-DAGENTOS_TEST_HOST` and
//! run individually.  Output is in TAP (Test Anything Protocol) format.
//!
//! Exit code:
//!   0  — all suites passed
//!   1  — one or more suites failed or a compile error occurred

use crate::TestApiArgs;
use anyhow::{bail, Context, Result};
use std::path::{Path, PathBuf};

/// Ordered list of API test source files.
///
/// Every file in `tests/api/` that starts with `test_` must appear here.
/// Add new files to the list when you add a new service test.
const TEST_FILES: &[&str] = &[
    "test_msgbus.c",
    "test_capstore.c",
    "test_memfs.c",
    "test_logsvc.c",
    "test_vibeos.c",
    "test_cap_audit.c",
];

pub fn run(args: &TestApiArgs) -> Result<()> {
    let repo_root = repo_root()?;
    let tests_dir = repo_root.join("tests").join("api");

    // Resolve C compiler: honour CC env var, fall back to `cc`.
    let cc = std::env::var("CC").unwrap_or_else(|_| "cc".to_string());

    // Build temporary output directory under the system temp dir.
    let build_dir = {
        let mut d = std::env::temp_dir();
        d.push(format!("agentos-api-tests-{}", std::process::id()));
        d
    };
    std::fs::create_dir_all(&build_dir).context("failed to create API test build directory")?;

    println!("[xtask:test-api] compiler: {}", cc);
    println!("[xtask:test-api] test dir: {}", tests_dir.display());
    println!("[xtask:test-api] build dir: {}", build_dir.display());
    println!();

    let mut passed = 0usize;
    let mut failed = 0usize;
    let mut errors = 0usize;

    for &file in TEST_FILES {
        let src = tests_dir.join(file);
        let stem = file.trim_end_matches(".c");
        let bin = build_dir.join(stem);

        // ── Compile ──────────────────────────────────────────────────────
        let compile_status = std::process::Command::new(&cc)
            .args([
                "-DAGENTOS_TEST_HOST",
                &format!("-I{}", tests_dir.display()),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-o",
                bin.to_str().unwrap(),
                src.to_str().unwrap(),
            ])
            .status()
            .with_context(|| format!("failed to invoke C compiler for {}", file))?;

        if !compile_status.success() {
            eprintln!("[test-api] COMPILE ERROR: {}", file);
            errors += 1;
            continue;
        }

        // ── Run ───────────────────────────────────────────────────────────
        println!("[test-api] running {}", stem);

        let run_output = std::process::Command::new(&bin)
            .output()
            .with_context(|| format!("failed to execute {}", bin.display()))?;

        let stdout = String::from_utf8_lossy(&run_output.stdout);
        let stderr = String::from_utf8_lossy(&run_output.stderr);

        // ── Parse TAP counts ─────────────────────────────────────────────
        let ok_count: usize = stdout
            .lines()
            .filter(|l| l.starts_with("ok ") && !l.contains("# TODO"))
            .count();
        let notok_count: usize = stdout.lines().filter(|l| l.starts_with("not ok ")).count();
        let todo_count: usize = stdout.lines().filter(|l| l.contains("# TODO")).count();

        // Print TAP output if verbose or suite failed.
        let suite_passed = run_output.status.success();
        if args.verbose || !suite_passed {
            for line in stdout.lines() {
                println!("  {}", line);
            }
            if !stderr.is_empty() {
                for line in stderr.lines() {
                    eprintln!("  {}", line);
                }
            }
        } else {
            // Print just the summary lines (lines starting with '#')
            for line in stdout.lines() {
                if line.starts_with('#') {
                    println!("  {}", line);
                }
            }
        }

        let todo_note = if todo_count > 0 {
            format!("  ({} TODO)", todo_count)
        } else {
            String::new()
        };

        if suite_passed {
            println!(
                "[test-api] PASS  {} — {} ok, {} not-ok{}",
                stem, ok_count, notok_count, todo_note
            );
            passed += 1;
        } else {
            println!(
                "[test-api] FAIL  {} — {} ok, {} not-ok{}",
                stem, ok_count, notok_count, todo_note
            );
            failed += 1;
        }
        println!();
    }

    // ── Cleanup ───────────────────────────────────────────────────────────
    let _ = std::fs::remove_dir_all(&build_dir);

    // ── Summary ───────────────────────────────────────────────────────────
    println!("═══════════════════════════════════════════════");
    println!("  API test suites:  {} total", TEST_FILES.len());
    println!("  Passed:           {}", passed);
    println!("  Failed:           {}", failed);
    println!("  Compile errors:   {}", errors);
    println!("═══════════════════════════════════════════════");

    if failed > 0 || errors > 0 {
        bail!("{} suite(s) failed, {} compile error(s)", failed, errors);
    }

    println!("ALL API TESTS PASSED");
    Ok(())
}

/// Walk up from the current directory until we find the repo root
/// (identified by Cargo.toml at the workspace level).
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
    Ok(Path::new(&root).to_path_buf())
}
