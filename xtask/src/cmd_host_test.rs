//! `cargo xtask test` — compile and run agentOS host-side test suites.
//!
//! Compiles every test suite under `tests/api/` and `tests/integration/`
//! using the system C compiler with `-DAGENTOS_TEST_HOST`.  Each suite is
//! compiled into a standalone binary, executed, and its TAP output parsed.
//!
//! Exit code:
//!   0  — all suites passed (or no suites were found/requested)
//!   1  — one or more suites failed or a compile error occurred
//!
//! Flags:
//!   --suite <name>       Run only the named suite
//!   --compiler <path>    Override the C compiler (default: value of CC or "cc")
//!   --verbose / -v       Print full TAP output for every suite, not just failures
//!   --hardware           Also run the QEMU-backed hardware test suite (disabled by default)

use crate::HostTestArgs;
use anyhow::{Context, Result, bail};
use std::path::{Path, PathBuf};

// ---------------------------------------------------------------------------
// Suite definitions
// ---------------------------------------------------------------------------

/// A host-compilable test suite.
struct Suite {
    /// Short name used for `--suite <name>` selection and result reporting.
    name: &'static str,
    /// Source files, relative to the repo root.  The first entry is the
    /// primary test file; remaining entries are implementation files pulled in
    /// alongside it.
    sources: &'static [&'static str],
}

/// All known host-side test suites.
///
/// Only suites whose primary source file actually exists on disk are compiled
/// and run; the remainder are silently skipped.  This lets the list stay
/// stable as new test files are added incrementally.
const SUITES: &[Suite] = &[
    Suite {
        name: "test_msgbus",
        sources: &["tests/api/test_msgbus.c"],
    },
    Suite {
        name: "test_capstore",
        sources: &["tests/api/test_capstore.c"],
    },
    Suite {
        name: "test_memfs",
        sources: &["tests/api/test_memfs.c"],
    },
    Suite {
        name: "test_logsvc",
        sources: &["tests/api/test_logsvc.c"],
    },
    Suite {
        name: "test_vibeos",
        sources: &["tests/api/test_vibeos.c"],
    },
    // NOTE: tests/integration/ suites include harness/test_framework.h which
    // depends on <microkit.h> from the Microkit SDK.  Those suites are
    // on-target only and are NOT compiled by the host-side runner.
    // They run via the QEMU boot path (xtask qemu-test) or the CI
    // contract-tests job which provisions the Microkit SDK.
];

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

pub fn run(args: &HostTestArgs) -> Result<()> {
    let repo_root = repo_root()?;

    // Resolve C compiler: CLI flag > CC env var > "cc".
    let cc = args
        .compiler
        .clone()
        .or_else(|| std::env::var("CC").ok())
        .unwrap_or_else(|| "cc".to_string());

    // Build a temporary output directory for compiled binaries.
    let build_dir = repo_root.join("target").join("test-bins");
    std::fs::create_dir_all(&build_dir)
        .context("failed to create test binary output directory")?;

    println!("[xtask:test] compiler : {}", cc);
    println!("[xtask:test] repo root: {}", repo_root.display());
    println!("[xtask:test] bin dir  : {}", build_dir.display());
    if let Some(ref suite) = args.suite {
        println!("[xtask:test] suite    : {}", suite);
    }
    println!();

    // Collect suites to run, filtered by --suite if given.
    let selected: Vec<&Suite> = SUITES
        .iter()
        .filter(|s| {
            args.suite
                .as_deref()
                .map(|wanted| s.name == wanted)
                .unwrap_or(true)
        })
        .collect();

    if selected.is_empty() {
        if let Some(ref name) = args.suite {
            bail!("unknown suite {:?} — available suites: {}",
                  name,
                  SUITES.iter().map(|s| s.name).collect::<Vec<_>>().join(", "));
        }
        println!("[xtask:test] no suites found");
        return Ok(());
    }

    // ---------------------------------------------------------------------------
    // Compile & run each suite
    // ---------------------------------------------------------------------------

    let mut passed  = 0usize;
    let mut failed  = 0usize;
    let mut skipped = 0usize;
    let mut errors  = 0usize;

    // Include paths always passed to the compiler.
    let include_root    = repo_root.join("kernel").join("agentos-root-task").join("include");
    let include_harness = repo_root.join("tests").join("harness");
    let include_api     = repo_root.join("tests").join("api");

    for suite in &selected {
        let primary_src = repo_root.join(suite.sources[0]);

        // Skip suites whose primary source file does not exist yet.
        if !primary_src.exists() {
            println!("[xtask:test] SKIP  {} — source not found: {}",
                     suite.name, primary_src.display());
            skipped += 1;
            continue;
        }

        let bin = build_dir.join(suite.name);

        // ── Compile ──────────────────────────────────────────────────────────
        let mut cmd = std::process::Command::new(&cc);
        cmd.args([
            "-DAGENTOS_TEST_HOST",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-o",
            bin.to_str().unwrap(),
        ]);

        // Include paths
        cmd.arg(format!("-I{}", include_root.display()));
        cmd.arg(format!("-I{}", include_harness.display()));
        cmd.arg(format!("-I{}", include_api.display()));

        // Source files (primary + any kernel implementation files)
        for &src in suite.sources {
            let p = repo_root.join(src);
            if p.exists() {
                cmd.arg(p.to_str().unwrap());
            }
        }

        let compile_out = cmd.output()
            .with_context(|| format!("failed to invoke C compiler for suite {}", suite.name))?;

        if !compile_out.status.success() {
            eprintln!("[xtask:test] COMPILE ERROR: {}", suite.name);
            if !compile_out.stderr.is_empty() {
                eprintln!("{}", String::from_utf8_lossy(&compile_out.stderr));
            }
            errors += 1;
            println!("[xtask:test] FAIL  {} (compile error)", suite.name);
            println!();
            continue;
        }

        // ── Run ──────────────────────────────────────────────────────────────
        let run_out = std::process::Command::new(&bin)
            .output()
            .with_context(|| format!("failed to execute test binary for suite {}", suite.name))?;

        let stdout = String::from_utf8_lossy(&run_out.stdout);
        let stderr = String::from_utf8_lossy(&run_out.stderr);

        // Count TAP results.
        let ok_count: usize = stdout
            .lines()
            .filter(|l| l.starts_with("ok ") && !l.contains("# TODO"))
            .count();
        let not_ok_count: usize = stdout
            .lines()
            .filter(|l| l.starts_with("not ok "))
            .count();
        let todo_count: usize = stdout.lines().filter(|l| l.contains("# TODO")).count();

        let suite_passed = run_out.status.success() && not_ok_count == 0;

        // Print output based on verbosity / failure.
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
            // Quiet mode: only print diagnostic (comment) lines.
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
                "[xtask:test] PASS  {} — {} ok, {} not-ok{}",
                suite.name, ok_count, not_ok_count, todo_note
            );
            passed += 1;
        } else {
            println!(
                "[xtask:test] FAIL  {} — {} ok, {} not-ok{}",
                suite.name, ok_count, not_ok_count, todo_note
            );
            failed += 1;
        }
        println!();
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    // Leave binaries in place for debugging; CI can wipe target/ separately.

    // ── Summary table ─────────────────────────────────────────────────────────
    let total = selected.len();
    println!("┌─────────────────────────────────────────────────────┐");
    println!("│  agentOS host-side test results                     │");
    println!("├─────────────────────────────────────────────────────┤");
    println!("│  Total suites:   {:>3}                               │", total);
    println!("│  Passed:         {:>3}                               │", passed);
    println!("│  Failed:         {:>3}                               │", failed);
    println!("│  Compile errors: {:>3}                               │", errors);
    println!("│  Skipped:        {:>3}  (source not found)           │", skipped);
    println!("└─────────────────────────────────────────────────────┘");

    if failed > 0 || errors > 0 {
        bail!(
            "{} suite(s) failed, {} compile error(s)",
            failed,
            errors
        );
    }

    if passed > 0 {
        println!("ALL HOST TESTS PASSED");
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn repo_root() -> Result<PathBuf> {
    let output = std::process::Command::new("git")
        .args(["rev-parse", "--show-toplevel"])
        .output()
        .context("failed to run git rev-parse")?;
    anyhow::ensure!(output.status.success(), "not in a git repository");
    let root = String::from_utf8(output.stdout)
        .context("git output is not valid UTF-8")?
        .trim()
        .to_string();
    Ok(Path::new(&root).to_path_buf())
}
