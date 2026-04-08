use crate::{BumpKind, ReleaseArgs};
use anyhow::Context;
use std::path::Path;

pub fn run(args: &ReleaseArgs) -> anyhow::Result<()> {
    let repo_root = repo_root()?;
    let workspace_toml = repo_root.join("Cargo.toml");

    let current = read_workspace_version(&workspace_toml)
        .context("failed to read workspace version")?;
    println!("[xtask:release] Current version: {}", current);

    let next = bump_version(&current, args.bump.clone())?;
    println!("[xtask:release] Next version:    {} ({:?})", next, args.bump);

    if args.dry_run {
        println!("[xtask:release] --dry-run: would commit and tag v{}", next);
        return Ok(());
    }

    // Update version in all member Cargo.tomls that carry a [package] version
    update_versions(&repo_root, &workspace_toml, &current, &next)?;

    // git add -A && git commit
    let status = std::process::Command::new("git")
        .args(["add", "-A"])
        .current_dir(&repo_root)
        .status()
        .context("git add failed")?;
    anyhow::ensure!(status.success(), "git add -A failed");

    let commit_msg = format!("chore: release v{}", next);
    let status = std::process::Command::new("git")
        .args(["commit", "-m", &commit_msg])
        .current_dir(&repo_root)
        .status()
        .context("git commit failed")?;
    anyhow::ensure!(status.success(), "git commit failed");

    // git tag
    let tag = format!("v{}", next);
    let status = std::process::Command::new("git")
        .args(["tag", &tag])
        .current_dir(&repo_root)
        .status()
        .context("git tag failed")?;
    anyhow::ensure!(status.success(), "git tag {} failed", tag);

    println!(
        "[xtask:release] Release v{} committed and tagged as {}.",
        next, tag
    );
    println!("  To push: git push && git push --tags");

    Ok(())
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

fn read_workspace_version(workspace_toml: &Path) -> anyhow::Result<String> {
    let content = std::fs::read_to_string(workspace_toml)
        .with_context(|| format!("failed to read {}", workspace_toml.display()))?;

    let doc: toml::Value = content.parse().context("failed to parse workspace Cargo.toml")?;

    // Try [workspace.package].version first
    if let Some(v) = doc
        .get("workspace")
        .and_then(|w| w.get("package"))
        .and_then(|p| p.get("version"))
        .and_then(|v| v.as_str())
    {
        return Ok(v.to_string());
    }

    // Fall back to [package].version
    if let Some(v) = doc
        .get("package")
        .and_then(|p| p.get("version"))
        .and_then(|v| v.as_str())
    {
        return Ok(v.to_string());
    }

    // Scan git tags as last resort
    let output = std::process::Command::new("git")
        .args(["tag", "-l", "v*", "--sort=version:refname"])
        .output()
        .context("git tag failed")?;
    let tags = String::from_utf8_lossy(&output.stdout);
    if let Some(last) = tags.lines().last() {
        let v = last.trim_start_matches('v');
        if !v.is_empty() {
            return Ok(v.to_string());
        }
    }

    Ok("0.0.0".to_string())
}

fn update_versions(
    repo_root: &Path,
    workspace_toml: &Path,
    current: &str,
    next: &str,
) -> anyhow::Result<()> {
    // Update workspace Cargo.toml
    update_version_in_file(workspace_toml, current, next)?;

    // Update all member Cargo.tomls
    let output = std::process::Command::new("cargo")
        .args(["metadata", "--no-deps", "--format-version=1"])
        .current_dir(repo_root)
        .output()
        .context("cargo metadata failed")?;

    if output.status.success() {
        let meta: serde_json::Value =
            serde_json::from_slice(&output.stdout).context("failed to parse cargo metadata")?;

        if let Some(packages) = meta.get("packages").and_then(|p| p.as_array()) {
            for pkg in packages {
                if let Some(manifest) = pkg.get("manifest_path").and_then(|m| m.as_str()) {
                    let manifest_path = Path::new(manifest);
                    // Skip the workspace root itself (already updated)
                    if manifest_path == workspace_toml {
                        continue;
                    }
                    if let Ok(content) = std::fs::read_to_string(manifest_path) {
                        if content.contains(&format!("version = \"{}\"", current)) {
                            update_version_in_file(manifest_path, current, next)?;
                        }
                    }
                }
            }
        }
    }

    Ok(())
}

fn update_version_in_file(path: &Path, current: &str, next: &str) -> anyhow::Result<()> {
    let content = std::fs::read_to_string(path)
        .with_context(|| format!("failed to read {}", path.display()))?;

    let old = format!("version = \"{}\"", current);
    let new = format!("version = \"{}\"", next);

    if content.contains(&old) {
        let updated = content.replacen(&old, &new, 1);
        std::fs::write(path, updated)
            .with_context(|| format!("failed to write {}", path.display()))?;
        println!("[xtask:release] Updated {}", path.display());
    }

    Ok(())
}

pub fn bump_version(current: &str, kind: BumpKind) -> anyhow::Result<String> {
    let parts: Vec<&str> = current.split('.').collect();
    anyhow::ensure!(
        parts.len() == 3,
        "version \"{}\" is not in major.minor.patch format",
        current
    );

    let mut major: u64 = parts[0].parse().context("invalid major version")?;
    let mut minor: u64 = parts[1].parse().context("invalid minor version")?;
    let mut patch: u64 = parts[2].parse().context("invalid patch version")?;

    match kind {
        BumpKind::Major => {
            major += 1;
            minor = 0;
            patch = 0;
        }
        BumpKind::Minor => {
            minor += 1;
            patch = 0;
        }
        BumpKind::Patch => {
            patch += 1;
        }
    }

    Ok(format!("{}.{}.{}", major, minor, patch))
}
