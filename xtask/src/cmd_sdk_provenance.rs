//! Report the provenance of every vendored external source.
//!
//! This replaces the instruction "go check the SHA against upstream yourself",
//! which stopped being a durable instruction the moment an upstream was
//! observed deleting a referenced commit (`upload-pack: not our ref`, recorded
//! in docs/superpowers/specs/2026-10-08-riscv-he-spike-findings.md).
//!
//! What this prints is deliberately narrow.  It establishes:
//!
//!   * which upstream repository and commit each dependency claims to be,
//!   * that this repository's recorded gitlink is exactly that commit, so the
//!     claim is bound by git's own object integrity rather than by prose,
//!   * that the agentOS-controlled mirror actually contains that commit, so
//!     the build does not depend on upstream still serving it,
//!   * the size and content of the agentOS delta applied on top of it, and
//!     that the delta still applies cleanly to the vendored base,
//!   * and, when the network is available, whether upstream still serves that
//!     commit -- reported, never enforced.
//!
//! It does not establish that the upstream commit is itself trustworthy, that
//! the mirror matches upstream byte for byte (a reviewer does that by fetching
//! both and diffing), or that the built kernel is correct.  Those claims
//! belong to the reader, to `git diff`, and to tools/sdk/cr2-kernels.sha256
//! respectively.

use crate::SdkProvenanceArgs;
use anyhow::{Context, Result};
use std::path::{Path, PathBuf};
use std::process::Command;

const MANIFEST: &str = "tools/sdk/vendor.manifest";

#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct Dependency {
    pub name: String,
    pub upstream: String,
    pub mirror: String,
    pub path: String,
    pub commit: String,
    pub patch: String,
}

/// Parse the blank-line-separated `key = value` records of the manifest.
///
/// Unknown keys are an error rather than a shrug: a typo in `commit` that is
/// silently ignored would leave the pin unchecked, which is the one failure
/// this whole mechanism exists to prevent.
pub fn parse_manifest(text: &str) -> Result<Vec<Dependency>> {
    let mut deps = Vec::new();
    let mut current: Option<Dependency> = None;
    for (index, raw) in text.lines().enumerate() {
        let line = raw.trim();
        let number = index + 1;
        if line.is_empty() || line.starts_with('#') {
            if line.is_empty() {
                if let Some(dep) = current.take() {
                    deps.push(dep);
                }
            }
            continue;
        }
        let (key, value) = line
            .split_once('=')
            .with_context(|| format!("{MANIFEST}:{number}: expected `key = value`"))?;
        let key = key.trim();
        let value = value.trim().to_string();
        if key == "name" {
            if let Some(dep) = current.take() {
                deps.push(dep);
            }
            current = Some(Dependency {
                name: value,
                ..Default::default()
            });
            continue;
        }
        let dep = current
            .as_mut()
            .with_context(|| format!("{MANIFEST}:{number}: `{key}` before any `name`"))?;
        match key {
            "upstream" => dep.upstream = value,
            "mirror" => dep.mirror = value,
            "path" => dep.path = value,
            "commit" => dep.commit = value,
            "patch" => dep.patch = value,
            other => anyhow::bail!("{MANIFEST}:{number}: unknown key `{other}`"),
        }
    }
    if let Some(dep) = current.take() {
        deps.push(dep);
    }
    for dep in &deps {
        for (label, value) in [
            ("upstream", &dep.upstream),
            ("mirror", &dep.mirror),
            ("path", &dep.path),
            ("commit", &dep.commit),
        ] {
            anyhow::ensure!(
                !value.is_empty(),
                "{MANIFEST}: dependency `{}` has no `{label}`",
                dep.name
            );
        }
        anyhow::ensure!(
            dep.commit.len() == 40 && dep.commit.chars().all(|c| c.is_ascii_hexdigit()),
            "{MANIFEST}: dependency `{}` commit `{}` is not a full 40-hex SHA-1",
            dep.name,
            dep.commit
        );
    }
    anyhow::ensure!(!deps.is_empty(), "{MANIFEST}: declares no dependencies");
    Ok(deps)
}

fn repo_root() -> Result<PathBuf> {
    let output = Command::new("git")
        .args(["rev-parse", "--show-toplevel"])
        .output()
        .context("failed to run git rev-parse")?;
    anyhow::ensure!(output.status.success(), "not in a git repository");
    Ok(PathBuf::from(
        String::from_utf8(output.stdout)
            .context("git output is not utf-8")?
            .trim(),
    ))
}

fn git(root: &Path, args: &[&str]) -> Result<std::process::Output> {
    Command::new("git")
        .current_dir(root)
        .args(args)
        .output()
        .with_context(|| format!("failed to run git {}", args.join(" ")))
}

/// The commit this repository's own tree records for `path`.
///
/// `git ls-files -s` reads the index entry, i.e. the gitlink that is part of
/// the committed tree -- not whatever the submodule working copy happens to be
/// checked out at. That distinction is the point: the pin is a property of
/// this repository's history, not of a developer's scratch state.
fn gitlink(root: &Path, path: &str) -> Result<Option<String>> {
    let output = git(root, &["ls-files", "-s", "--", path])?;
    if !output.status.success() {
        return Ok(None);
    }
    let text = String::from_utf8(output.stdout).context("git ls-files output is not utf-8")?;
    for line in text.lines() {
        let mut fields = line.split_whitespace();
        let mode = fields.next().unwrap_or_default();
        let object = fields.next().unwrap_or_default();
        if mode == "160000" && object.len() == 40 {
            return Ok(Some(object.to_string()));
        }
    }
    Ok(None)
}

struct Report {
    failures: Vec<String>,
}

impl Report {
    fn fail(&mut self, message: String) {
        println!("    STATUS       FAIL  {message}");
        self.failures.push(message);
    }
}

fn check_upstream_reachable(root: &Path, dep: &Dependency) -> Result<()> {
    // The same negotiation `git fetch` would perform, with no objects written.
    // A deleted upstream ref answers "upload-pack: not our ref"; that is now an
    // expected, non-fatal state, because the mirror is what the build uses.
    let output = git(
        root,
        &[
            "-c",
            "advice.detachedHead=false",
            "fetch",
            "--no-tags",
            "--dry-run",
            "--depth=1",
            &dep.upstream,
            &dep.commit,
        ],
    )?;
    if output.status.success() {
        println!("    UPSTREAM-SHA still served by {}", dep.upstream);
    } else {
        let stderr = String::from_utf8_lossy(&output.stderr);
        let detail = stderr
            .lines()
            .find(|line| line.contains("not our ref") || line.contains("fatal"))
            .unwrap_or("fetch failed")
            .trim();
        println!(
            "    UPSTREAM-SHA NOT retrievable from {} ({detail})",
            dep.upstream
        );
        println!(
            "                 Not an error. The mirror holds this commit, which is why\n\
             \x20                this project vendors rather than fetching upstream."
        );
    }
    Ok(())
}

pub fn run(args: &SdkProvenanceArgs) -> Result<()> {
    let root = repo_root()?;
    let manifest_path = root.join(MANIFEST);
    let text = std::fs::read_to_string(&manifest_path)
        .with_context(|| format!("failed to read {}", manifest_path.display()))?;
    let deps = parse_manifest(&text)?;

    println!("agentOS vendored source provenance");
    println!("manifest: {MANIFEST}");
    if args.offline {
        println!("mode:     offline (upstream reachability not queried)");
    }
    println!();

    let mut report = Report { failures: vec![] };

    for dep in &deps {
        println!("[{}]", dep.name);
        println!("    UPSTREAM     {}", dep.upstream);
        println!("    MIRROR       {}", dep.mirror);
        println!("    PATH         {}", dep.path);
        println!("    COMMIT       {}", dep.commit);

        match gitlink(&root, &dep.path)? {
            Some(sha) if sha == dep.commit => {
                println!("    GITLINK      {sha} (matches manifest)");
            }
            Some(sha) => report.fail(format!(
                "{}: gitlink {sha} does not match manifest commit {}",
                dep.name, dep.commit
            )),
            None => report.fail(format!(
                "{}: no submodule gitlink recorded at {}",
                dep.name, dep.path
            )),
        }

        let checkout = root.join(&dep.path);
        let initialised = checkout.join(".git").exists();
        if !initialised {
            println!("    MIRROR-CLONE not checked out; run `make submodules`");
            println!("    DELTA        not evaluated (submodule absent)");
            println!();
            continue;
        }

        let spec = format!("{}^{{commit}}", dep.commit);
        let present = git(&checkout, &["cat-file", "-e", &spec])?.status.success();
        if present {
            println!("    MIRROR-CLONE contains {} (object verified)", dep.commit);
        } else {
            report.fail(format!(
                "{}: the vendored mirror clone does not contain {}",
                dep.name, dep.commit
            ));
        }

        if dep.patch.is_empty() {
            println!("    DELTA        none; sources are used unmodified");
        } else {
            let patch = root.join(&dep.patch);
            anyhow::ensure!(patch.is_file(), "{}: missing patch {}", dep.name, dep.patch);
            println!("    DELTA        {}", dep.patch);
            let stat = git(&root, &["apply", "--stat", patch.to_str().unwrap()])?;
            for line in String::from_utf8_lossy(&stat.stdout).lines() {
                println!("                 {}", line.trim_end());
            }
            // Does the delta still describe this exact base? `--check` against
            // the pinned worktree is the question a reviewer actually cares
            // about: "is the readable patch still the whole difference?"
            let head = git(&checkout, &["rev-parse", "HEAD"])?;
            let head = String::from_utf8_lossy(&head.stdout).trim().to_string();
            if head != dep.commit {
                println!(
                    "    DELTA-APPLY  not checked; submodule worktree is at {head},\n\
                     \x20                not the pinned {}. Run `make submodules`.",
                    dep.commit
                );
            } else {
                let clean = git(&checkout, &["apply", "--check", patch.to_str().unwrap()])?;
                if clean.status.success() {
                    println!("    DELTA-APPLY  applies cleanly to the pinned source");
                } else {
                    report.fail(format!(
                        "{}: {} no longer applies to {}",
                        dep.name, dep.patch, dep.commit
                    ));
                }
            }
        }

        if !args.offline {
            check_upstream_reachable(&root, dep)?;
        }
        println!();
    }

    println!("Scope of this report");
    println!("  Establishes: the commit each dependency claims, that this repository's");
    println!("               tree is bound to exactly that commit, that the mirror holds");
    println!("               it, and the full extent of the agentOS delta.");
    println!("  Does NOT establish: that upstream's code is correct, that the mirror is");
    println!("               byte-identical to upstream (fetch both and diff the SHA),");
    println!("               or that the built kernel is correct (see cr2-kernels.sha256).");
    println!("  agentOS is answerable for these sources: see docs/sdk-provenance.md.");

    if !report.failures.is_empty() {
        anyhow::bail!("{} provenance check(s) failed", report.failures.len());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = "\
# comment
name = sel4
upstream = https://example.invalid/sel4.git
mirror = https://example.invalid/mirror.git
path = vendor/sel4
commit = e60776acc31097ca063806c257f07a3ec05eacf8
patch = tools/sdk/patches/sel4-e60776ac-cr2.patch

name = microkit
upstream = https://example.invalid/microkit.git
mirror = https://example.invalid/microkit-mirror.git
path = vendor/microkit
commit = ec86afdcd662b5976d11d4994acf1b11a2979882
patch =
";

    #[test]
    fn parses_every_record() {
        let deps = parse_manifest(SAMPLE).expect("sample parses");
        assert_eq!(deps.len(), 2);
        assert_eq!(deps[0].name, "sel4");
        assert_eq!(deps[0].path, "vendor/sel4");
        assert_eq!(deps[0].patch, "tools/sdk/patches/sel4-e60776ac-cr2.patch");
        assert_eq!(deps[1].name, "microkit");
        assert!(deps[1].patch.is_empty());
    }

    #[test]
    fn the_shipped_manifest_parses_and_names_the_pinned_dependencies() {
        let text = std::fs::read_to_string(
            Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("..")
                .join(MANIFEST),
        )
        .expect("tools/sdk/vendor.manifest is readable");
        let deps = parse_manifest(&text).expect("shipped manifest parses");
        let sel4 = deps.iter().find(|d| d.name == "sel4").expect("sel4 record");
        // These are the commits tools/sdk/candidate.mk builds from. If one is
        // changed in only one of the two places, this fails rather than
        // producing an SDK whose provenance report describes a different tree.
        assert_eq!(sel4.commit, "e60776acc31097ca063806c257f07a3ec05eacf8");
        let microkit = deps
            .iter()
            .find(|d| d.name == "microkit")
            .expect("microkit record");
        assert_eq!(microkit.commit, "ec86afdcd662b5976d11d4994acf1b11a2979882");
    }

    #[test]
    fn rejects_a_truncated_commit() {
        let text = SAMPLE.replace("e60776acc31097ca063806c257f07a3ec05eacf8", "e60776ac");
        assert!(parse_manifest(&text).is_err());
    }

    #[test]
    fn rejects_an_unknown_key() {
        let text = format!("{SAMPLE}\nname = x\nbranch = main\n");
        assert!(parse_manifest(&text).is_err());
    }

    #[test]
    fn rejects_a_record_with_no_commit() {
        let text = "name = x\nupstream = u\nmirror = m\npath = p\n";
        assert!(parse_manifest(text).is_err());
    }
}
