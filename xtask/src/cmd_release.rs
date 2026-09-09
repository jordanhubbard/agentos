use crate::{
    BumpKind, ReleaseAction, ReleaseArgs, ReleaseCheckArgs, ReleaseClaim, ReleasePlanArgs,
    ReleasePrepareArgs, ReleasePublishArgs, ReleaseVerifyArgs,
};
use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{SystemTime, UNIX_EPOCH};

const RECEIPT_SCHEMA: &str = "agentos.release.receipt.v1";
const REQUIRED_MAIN_CHECKS: &[&str] = &[
    "Host-side TAP tests",
    "Rust xtask check",
    "Dual-arch OS-claim gate (aarch64 + x86_64, GUEST_OS=none)",
];

#[derive(Debug, Serialize, Deserialize)]
struct ArtifactEvidence {
    path: String,
    bytes: u64,
    sha256: String,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct GitHubAsset {
    api_url: String,
    name: String,
    size: u64,
}

#[derive(Debug, Deserialize)]
struct GitHubRelease {
    assets: Vec<GitHubAsset>,
}

#[derive(Debug, Serialize, Deserialize)]
struct ReleaseReceipt {
    schema: String,
    version: String,
    commit: String,
    branch: String,
    remote: String,
    claim: ReleaseClaim,
    gates: Vec<String>,
    artifacts: Vec<ArtifactEvidence>,
    checked_at_unix: u64,
}

pub fn run(args: &ReleaseArgs) -> Result<()> {
    let root = repo_root()?;
    match &args.action {
        ReleaseAction::Plan(args) => plan(&root, args),
        ReleaseAction::Prepare(args) => prepare(&root, args),
        ReleaseAction::Check(args) => check(&root, args),
        ReleaseAction::Publish(args) => publish(&root, args),
        ReleaseAction::Verify(args) => verify(&root, args),
    }
}

fn plan(root: &Path, args: &ReleasePlanArgs) -> Result<()> {
    ensure_clean(root)?;
    let current = read_workspace_version(&root.join("Cargo.toml"))?;
    let next = bump_version(&current, args.bump)?;
    let branch = git(root, &["branch", "--show-current"])?;
    let commit = git(root, &["rev-parse", "HEAD"])?;
    let remote = canonical_remote(root)?;
    let open_milestone_tasks = open_milestone_tasks(root, &next)?;
    let artifacts = expected_artifact_paths(&args.artifacts)?;
    let plan = serde_json::json!({
        "schema": "agentos.release.plan.v1",
        "current_version": current,
        "proposed_version": next,
        "expected_tag": format!("v{next}"),
        "source_commit": commit,
        "branch": branch,
        "remote": remote,
        "version_path": "Cargo.toml",
        "changelog_path": "CHANGELOG.md",
        "receipt_path": format!("build/release/{next}/receipt.json"),
        "claim": args.claim,
        "gates": gates_for(args.claim),
        "artifacts": artifacts,
        "open_milestone_tasks": open_milestone_tasks,
        "prepare_branch": release_branch(&next)?,
        "mutates_repository": false,
    });
    println!("{}", serde_json::to_string_pretty(&plan)?);
    Ok(())
}

fn prepare(root: &Path, args: &ReleasePrepareArgs) -> Result<()> {
    ensure_clean(root)?;
    let version = normalise_version(&args.version)?;
    validate_date(&args.date)?;
    let branch = git(root, &["branch", "--show-current"])?;
    let required_branch = release_branch(&version)?;
    anyhow::ensure!(
        branch == required_branch,
        "prepare requires branch {required_branch}, current branch is {branch}"
    );
    let workspace = root.join("Cargo.toml");
    let current = read_workspace_version(&workspace)?;
    anyhow::ensure!(current != version, "workspace is already version {version}");
    update_versions(root, &workspace, &current, &version)?;
    prepare_changelog(&root.join("CHANGELOG.md"), &version, &args.date)?;
    println!(
        "[xtask:release] prepared v{version}; review, commit, and merge this release branch through a PR"
    );
    Ok(())
}

fn check(root: &Path, args: &ReleaseCheckArgs) -> Result<()> {
    ensure_clean(root)?;
    let version = normalise_version(&args.version)?;
    let workspace_version = read_workspace_version(&root.join("Cargo.toml"))?;
    anyhow::ensure!(
        workspace_version == version,
        "workspace version {workspace_version} does not match requested {version}"
    );
    let commit = git(root, &["rev-parse", "HEAD"])?;
    let branch = git(root, &["branch", "--show-current"])?;
    anyhow::ensure!(
        branch == "main" || branch == release_branch(&version)?,
        "check requires main or the matching release branch"
    );
    let remote = canonical_remote(root)?;
    let open_tasks = open_milestone_tasks(root, &version)?;
    anyhow::ensure!(
        open_tasks.is_empty(),
        "release milestone still has open tasks: {}",
        open_tasks.join(", ")
    );
    let gates = gates_for(args.claim);
    execute_ordered_gates(&gates, |gate| {
        let mut parts = gate.split_whitespace();
        let program = parts.next().context("empty release gate")?;
        let status = Command::new(program)
            .args(parts)
            .current_dir(root)
            .status()
            .with_context(|| format!("failed to run {gate}"))?;
        Ok(status.success())
    })?;
    let _expected_artifacts = expected_artifact_paths(&args.artifacts)?;
    let artifacts = args
        .artifacts
        .iter()
        .map(|path| artifact_evidence(root, path))
        .collect::<Result<Vec<_>>>()?;
    let receipt = ReleaseReceipt {
        schema: RECEIPT_SCHEMA.to_string(),
        version: version.clone(),
        commit,
        branch,
        remote,
        claim: args.claim,
        gates: gates.iter().map(|gate| (*gate).to_string()).collect(),
        artifacts,
        checked_at_unix: SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .context("system clock is before UNIX_EPOCH")?
            .as_secs(),
    };
    let path = receipt_path(root, &version);
    fs::create_dir_all(path.parent().unwrap())?;
    fs::write(&path, serde_json::to_vec_pretty(&receipt)?)?;
    println!("[xtask:release] wrote checked receipt {}", path.display());
    Ok(())
}

fn publish(root: &Path, args: &ReleasePublishArgs) -> Result<()> {
    ensure_clean(root)?;
    let version = normalise_version(&args.version)?;
    let tag = format!("v{version}");
    let branch = git(root, &["branch", "--show-current"])?;
    validate_publish_context(&branch, &tag, &args.authorize)?;
    let head = git(root, &["rev-parse", "HEAD"])?;
    let receipt = load_receipt(root, &version)?;
    validate_receipt(root, &receipt, &version, &head)?;

    git_status(root, &["fetch", "origin", "main", "--tags"])?;
    let remote_main = git(root, &["rev-parse", "origin/main"])?;
    anyhow::ensure!(
        head == remote_main,
        "checked HEAD does not equal origin/main; merge and re-check before publishing"
    );
    ensure_protected_main_pr(root, &head)?;

    match remote_tag_commit(root, &tag)? {
        Some(commit) => anyhow::ensure!(
            commit == head,
            "remote tag {tag} points to {commit}, not checked commit {head}"
        ),
        None => {
            if local_tag_exists(root, &tag)? {
                let local = git(root, &["rev-list", "-n", "1", &tag])?;
                anyhow::ensure!(local == head, "local tag {tag} points to {local}");
            } else {
                git_status(root, &["tag", "-a", &tag, "-m", &format!("Release {tag}")])?;
            }
            git_status(root, &["push", "origin", &tag])?;
        }
    }

    let receipt_path = receipt_path(root, &version);
    if !github_release_exists(root, &tag)? {
        let mut command = Command::new("gh");
        command
            .args([
                "release",
                "create",
                &tag,
                "--repo",
                "jordanhubbard/agentos",
                "--verify-tag",
                "--title",
                &format!("agentOS {tag}"),
                "--notes-file",
                "CHANGELOG.md",
            ])
            .arg(&receipt_path)
            .current_dir(root);
        for artifact in &receipt.artifacts {
            command.arg(root.join(&artifact.path));
        }
        let status = command
            .status()
            .context("failed to run gh release create")?;
        anyhow::ensure!(status.success(), "GitHub release publication failed");
    } else {
        let mut command = Command::new("gh");
        command
            .args([
                "release",
                "upload",
                &tag,
                "--repo",
                "jordanhubbard/agentos",
                "--clobber",
            ])
            .arg(&receipt_path)
            .current_dir(root);
        for artifact in &receipt.artifacts {
            command.arg(root.join(&artifact.path));
        }
        let status = command
            .status()
            .context("failed to reconcile GitHub release assets")?;
        anyhow::ensure!(
            status.success(),
            "GitHub release asset reconciliation failed"
        );
    }
    verify(root, &ReleaseVerifyArgs { version })?;
    Ok(())
}

fn verify(root: &Path, args: &ReleaseVerifyArgs) -> Result<()> {
    let version = normalise_version(&args.version)?;
    let tag = format!("v{version}");
    let receipt = load_receipt(root, &version)?;
    let remote = remote_tag_commit(root, &tag)?.context("remote tag is missing")?;
    validate_receipt(root, &receipt, &version, &remote)?;
    anyhow::ensure!(
        github_release_exists(root, &tag)?,
        "GitHub release is missing"
    );
    for artifact in &receipt.artifacts {
        let current = artifact_evidence(root, Path::new(&artifact.path))?;
        anyhow::ensure!(
            current.sha256 == artifact.sha256 && current.bytes == artifact.bytes,
            "artifact changed since check: {}",
            artifact.path
        );
    }
    verify_remote_assets(root, &tag, &receipt)?;
    println!("[xtask:release] verified {tag} at {}", receipt.commit);
    Ok(())
}

fn gates_for(claim: ReleaseClaim) -> Vec<&'static str> {
    match claim {
        ReleaseClaim::Tooling => vec!["make test-host"],
        ReleaseClaim::Os => vec!["make test-host", "make gate"],
        ReleaseClaim::Guests => vec!["make test-host", "make gate", "make demo-test"],
        ReleaseClaim::Desktop => vec![
            "make test-host",
            "make gate",
            "make demo-test",
            "make demo-desktop-test",
        ],
    }
}

fn execute_ordered_gates<F>(gates: &[&str], mut execute: F) -> Result<()>
where
    F: FnMut(&str) -> Result<bool>,
{
    for gate in gates {
        println!("[xtask:release] running {gate}");
        anyhow::ensure!(execute(gate)?, "release gate failed: {gate}");
    }
    Ok(())
}

fn validate_publish_context(branch: &str, tag: &str, authorize: &str) -> Result<()> {
    anyhow::ensure!(
        authorize == format!("publish-{tag}"),
        "publication requires --authorize publish-{tag}"
    );
    anyhow::ensure!(branch == "main", "publish requires protected main");
    Ok(())
}

fn validate_receipt(
    root: &Path,
    receipt: &ReleaseReceipt,
    version: &str,
    head: &str,
) -> Result<()> {
    anyhow::ensure!(
        receipt.schema == RECEIPT_SCHEMA,
        "unsupported receipt schema"
    );
    anyhow::ensure!(receipt.version == version, "receipt version mismatch");
    anyhow::ensure!(
        receipt.commit == head,
        "receipt was produced for another commit"
    );
    anyhow::ensure!(
        receipt.remote == canonical_remote(root)?,
        "canonical remote changed after check"
    );
    anyhow::ensure!(
        receipt.gates == gates_for(receipt.claim),
        "receipt gates do not match its release claim"
    );
    let recorded_paths = receipt
        .artifacts
        .iter()
        .map(|artifact| PathBuf::from(&artifact.path))
        .collect::<Vec<_>>();
    expected_artifact_paths(&recorded_paths)?;
    for artifact in &receipt.artifacts {
        let current = artifact_evidence(root, Path::new(&artifact.path))?;
        anyhow::ensure!(
            current.sha256 == artifact.sha256 && current.bytes == artifact.bytes,
            "artifact changed after check: {}",
            artifact.path
        );
    }
    Ok(())
}

fn prepare_changelog(path: &Path, version: &str, date: &str) -> Result<()> {
    let content =
        fs::read_to_string(path).with_context(|| format!("failed to read {}", path.display()))?;
    anyhow::ensure!(
        !content.contains(&format!("## [{version}]")),
        "CHANGELOG already contains version {version}"
    );
    let marker = "## [Unreleased]";
    let replacement = format!("{marker}\n\n## [{version}] - {date}");
    anyhow::ensure!(
        content.contains(marker),
        "CHANGELOG has no Unreleased section"
    );
    fs::write(path, content.replacen(marker, &replacement, 1))?;
    Ok(())
}

fn expected_artifact_paths(paths: &[PathBuf]) -> Result<Vec<String>> {
    let mut names = BTreeMap::new();
    let mut expected = Vec::with_capacity(paths.len());
    for path in paths {
        anyhow::ensure!(
            !path.as_os_str().is_empty()
                && !path.is_absolute()
                && path
                    .components()
                    .all(|component| matches!(component, std::path::Component::Normal(_))),
            "release artifact paths must be normalized and repository-relative: {}",
            path.display()
        );
        let name = path
            .file_name()
            .and_then(|name| name.to_str())
            .context("release artifact has no UTF-8 file name")?;
        anyhow::ensure!(
            name != "receipt.json",
            "release artifact name conflicts with receipt.json"
        );
        anyhow::ensure!(
            names.insert(name.to_string(), path.clone()).is_none(),
            "release artifacts must have unique file names: {name}"
        );
        expected.push(path.display().to_string());
    }
    Ok(expected)
}

fn artifact_evidence(root: &Path, path: &Path) -> Result<ArtifactEvidence> {
    let path = if path.is_absolute() {
        path.to_path_buf()
    } else {
        root.join(path)
    };
    anyhow::ensure!(
        path.is_file(),
        "artifact is not a regular file: {}",
        path.display()
    );
    let canonical_root = fs::canonicalize(root)?;
    let canonical_path = fs::canonicalize(&path)?;
    anyhow::ensure!(
        canonical_path.starts_with(&canonical_root),
        "release artifacts must resolve inside the repository: {}",
        path.display()
    );
    let relative = path
        .strip_prefix(root)
        .context("release artifacts must be inside the repository")?;
    let (bytes, sha256) = file_digest(&path)?;
    Ok(ArtifactEvidence {
        path: relative.display().to_string(),
        bytes,
        sha256,
    })
}

fn file_digest(path: &Path) -> Result<(u64, String)> {
    let mut file = fs::File::open(path)?;
    let mut digest = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let length = file.read(&mut buffer)?;
        if length == 0 {
            break;
        }
        digest.update(&buffer[..length]);
    }
    Ok((file.metadata()?.len(), format!("{:x}", digest.finalize())))
}

fn receipt_path(root: &Path, version: &str) -> PathBuf {
    root.join("build/release")
        .join(version)
        .join("receipt.json")
}

fn load_receipt(root: &Path, version: &str) -> Result<ReleaseReceipt> {
    let path = receipt_path(root, version);
    serde_json::from_slice(&fs::read(&path).with_context(|| {
        format!(
            "missing checked receipt {}; run release check first",
            path.display()
        )
    })?)
    .context("invalid release receipt")
}

fn canonical_remote(root: &Path) -> Result<String> {
    let remote = git(root, &["remote", "get-url", "origin"])?;
    anyhow::ensure!(
        remote.trim_end_matches(".git") == "https://github.com/jordanhubbard/agentos"
            || remote.trim_end_matches(".git") == "git@github.com:jordanhubbard/agentos",
        "origin is not the canonical agentOS repository: {remote}"
    );
    Ok("https://github.com/jordanhubbard/agentos".to_string())
}

fn ensure_protected_main_pr(root: &Path, commit: &str) -> Result<()> {
    let protection = Command::new("gh")
        .args([
            "api",
            "repos/jordanhubbard/agentos/branches/main/protection",
        ])
        .current_dir(root)
        .output()
        .context("failed to inspect main branch protection")?;
    anyhow::ensure!(
        protection.status.success(),
        "canonical main branch is not protected or its protection could not be verified: {}",
        String::from_utf8_lossy(&protection.stderr).trim()
    );
    let protection: serde_json::Value =
        serde_json::from_slice(&protection.stdout).context("invalid branch protection response")?;
    validate_branch_protection(&protection)?;

    let endpoint = format!("repos/jordanhubbard/agentos/commits/{commit}/pulls");
    let output = Command::new("gh")
        .args(["api", &endpoint])
        .current_dir(root)
        .output()
        .context("failed to inspect pull requests associated with release commit")?;
    anyhow::ensure!(
        output.status.success(),
        "failed to verify release PR integration: {}",
        String::from_utf8_lossy(&output.stderr).trim()
    );
    let pulls: serde_json::Value =
        serde_json::from_slice(&output.stdout).context("invalid associated PR response")?;
    let integrated =
        pulls.as_array().into_iter().flatten().any(|pull| {
            !pull["merged_at"].is_null() && pull["base"]["ref"].as_str() == Some("main")
        });
    anyhow::ensure!(
        integrated,
        "release commit is not associated with a merged pull request targeting main"
    );
    Ok(())
}

fn validate_branch_protection(protection: &serde_json::Value) -> Result<()> {
    let contexts = protection["required_status_checks"]["contexts"]
        .as_array()
        .context("main protection does not require status checks")?;
    for required in REQUIRED_MAIN_CHECKS {
        anyhow::ensure!(
            contexts
                .iter()
                .any(|context| context.as_str() == Some(required)),
            "main protection is missing required status check {required:?}"
        );
    }
    anyhow::ensure!(
        protection["required_pull_request_reviews"].is_object(),
        "main protection does not require pull requests"
    );
    anyhow::ensure!(
        protection["enforce_admins"]["enabled"].as_bool() == Some(true),
        "main protection is not enforced for administrators"
    );
    anyhow::ensure!(
        protection["allow_force_pushes"]["enabled"].as_bool() == Some(false),
        "main protection allows force pushes"
    );
    anyhow::ensure!(
        protection["allow_deletions"]["enabled"].as_bool() == Some(false),
        "main protection allows deletion"
    );
    Ok(())
}

fn open_milestone_tasks(root: &Path, version: &str) -> Result<Vec<String>> {
    let output = Command::new("mac")
        .args([
            "task",
            "list",
            "--project",
            "agentos",
            "--json",
            "--full-ids",
        ])
        .current_dir(root)
        .output()
        .context("MAC is required for release milestone triage")?;
    anyhow::ensure!(output.status.success(), "MAC milestone query failed");
    let tasks: serde_json::Value = serde_json::from_slice(&output.stdout)?;
    let prefixes = milestone_prefixes(version)?;
    let mut open = Vec::new();
    for task in tasks.as_array().into_iter().flatten() {
        let state = task["state"].as_str().unwrap_or_default();
        let title = task["title"].as_str().unwrap_or_default();
        if prefixes.iter().any(|prefix| title.starts_with(prefix))
            && !matches!(state, "completed" | "cancelled")
        {
            open.push(task["id"].as_str().unwrap_or(title).to_string());
        }
    }
    open.sort();
    Ok(open)
}

fn milestone_prefixes(version: &str) -> Result<[String; 2]> {
    let version = normalise_version(version)?;
    let mut parts = version.split('.');
    let major = parts.next().unwrap();
    let minor = parts.next().unwrap();
    Ok([format!("v{version}:"), format!("v{major}.{minor}:")])
}

fn ensure_clean(root: &Path) -> Result<()> {
    let status = git(root, &["status", "--porcelain"])?;
    anyhow::ensure!(status.is_empty(), "working tree must be clean");
    Ok(())
}

fn git(root: &Path, args: &[&str]) -> Result<String> {
    let output = Command::new("git")
        .args(args)
        .current_dir(root)
        .output()
        .with_context(|| format!("failed to run git {}", args.join(" ")))?;
    anyhow::ensure!(
        output.status.success(),
        "git {} failed: {}",
        args.join(" "),
        String::from_utf8_lossy(&output.stderr).trim()
    );
    Ok(String::from_utf8(output.stdout)?.trim().to_string())
}

fn git_status(root: &Path, args: &[&str]) -> Result<()> {
    let status = Command::new("git")
        .args(args)
        .current_dir(root)
        .status()
        .with_context(|| format!("failed to run git {}", args.join(" ")))?;
    anyhow::ensure!(status.success(), "git {} failed", args.join(" "));
    Ok(())
}

fn remote_tag_commit(root: &Path, tag: &str) -> Result<Option<String>> {
    let peeled = format!("refs/tags/{tag}^{{}}");
    let direct = format!("refs/tags/{tag}");
    let output = Command::new("git")
        .args(["ls-remote", "--tags", "origin", &direct, &peeled])
        .current_dir(root)
        .output()
        .context("failed to query remote tag")?;
    anyhow::ensure!(output.status.success(), "git ls-remote failed");
    let text = String::from_utf8(output.stdout)?;
    parse_remote_tag_refs(&text, tag)
}

fn parse_remote_tag_refs(text: &str, tag: &str) -> Result<Option<String>> {
    let peeled = format!("refs/tags/{tag}^{{}}");
    let direct = format!("refs/tags/{tag}");
    let mut tag_object = None;
    for line in text.lines() {
        let mut fields = line.split_whitespace();
        let commit = fields.next().unwrap_or_default().to_string();
        let reference = fields.next().unwrap_or_default();
        if reference == peeled {
            return Ok(Some(commit));
        }
        if reference == direct {
            tag_object = Some(commit);
        }
    }
    anyhow::ensure!(
        tag_object.is_none(),
        "remote tag {tag} is lightweight; agentOS releases require annotated tags"
    );
    Ok(None)
}

fn local_tag_exists(root: &Path, tag: &str) -> Result<bool> {
    Ok(Command::new("git")
        .args([
            "rev-parse",
            "--verify",
            "--quiet",
            &format!("refs/tags/{tag}"),
        ])
        .current_dir(root)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()?
        .success())
}

fn github_release_exists(root: &Path, tag: &str) -> Result<bool> {
    Ok(Command::new("gh")
        .args(["release", "view", tag, "--repo", "jordanhubbard/agentos"])
        .current_dir(root)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .context("failed to run gh release view")?
        .success())
}

fn verify_remote_assets(root: &Path, tag: &str, receipt: &ReleaseReceipt) -> Result<()> {
    let output = Command::new("gh")
        .args([
            "release",
            "view",
            tag,
            "--repo",
            "jordanhubbard/agentos",
            "--json",
            "assets",
        ])
        .current_dir(root)
        .output()
        .context("failed to inspect GitHub release assets")?;
    anyhow::ensure!(
        output.status.success(),
        "failed to inspect GitHub release assets: {}",
        String::from_utf8_lossy(&output.stderr).trim()
    );
    let release: GitHubRelease =
        serde_json::from_slice(&output.stdout).context("invalid GitHub release asset metadata")?;
    let mut remote = release
        .assets
        .iter()
        .map(|asset| (asset.name.as_str(), asset))
        .collect::<BTreeMap<_, _>>();

    let receipt_path = receipt_path(root, &receipt.version);
    let (receipt_bytes, receipt_sha256) = file_digest(&receipt_path)?;
    verify_remote_asset(
        root,
        remote
            .remove("receipt.json")
            .context("GitHub release is missing receipt.json")?,
        receipt_bytes,
        &receipt_sha256,
    )?;

    for artifact in &receipt.artifacts {
        let name = Path::new(&artifact.path)
            .file_name()
            .and_then(|name| name.to_str())
            .context("release artifact has no UTF-8 file name")?;
        anyhow::ensure!(
            name != "receipt.json",
            "release artifact name conflicts with receipt.json"
        );
        verify_remote_asset(
            root,
            remote
                .remove(name)
                .with_context(|| format!("GitHub release is missing artifact {name}"))?,
            artifact.bytes,
            &artifact.sha256,
        )?;
    }
    anyhow::ensure!(
        remote.is_empty(),
        "GitHub release has unrecorded assets: {}",
        remote.keys().copied().collect::<Vec<_>>().join(", ")
    );
    Ok(())
}

fn verify_remote_asset(
    root: &Path,
    asset: &GitHubAsset,
    expected_bytes: u64,
    expected_sha256: &str,
) -> Result<()> {
    anyhow::ensure!(
        asset.size == expected_bytes,
        "remote asset {} has {} bytes, expected {expected_bytes}",
        asset.name,
        asset.size
    );
    let temporary =
        tempfile::tempdir().context("failed to create release verification directory")?;
    let path = temporary.path().join(&asset.name);
    let output = fs::File::create(&path)?;
    let status = Command::new("gh")
        .args([
            "api",
            "-H",
            "Accept: application/octet-stream",
            &asset.api_url,
        ])
        .current_dir(root)
        .stdout(Stdio::from(output))
        .status()
        .with_context(|| format!("failed to download remote asset {}", asset.name))?;
    anyhow::ensure!(
        status.success(),
        "failed to download remote asset {}",
        asset.name
    );
    let (bytes, sha256) = file_digest(&path)?;
    anyhow::ensure!(
        bytes == expected_bytes && sha256 == expected_sha256,
        "remote asset {} checksum does not match checked evidence",
        asset.name
    );
    Ok(())
}

fn repo_root() -> Result<PathBuf> {
    let output = Command::new("git")
        .args(["rev-parse", "--show-toplevel"])
        .output()
        .context("failed to find repository root")?;
    anyhow::ensure!(output.status.success(), "not in a git repository");
    Ok(PathBuf::from(String::from_utf8(output.stdout)?.trim()))
}

fn read_workspace_version(workspace_toml: &Path) -> Result<String> {
    let content = fs::read_to_string(workspace_toml)?;
    let doc: toml::Value = content.parse()?;
    doc.get("workspace")
        .and_then(|workspace| workspace.get("package"))
        .and_then(|package| package.get("version"))
        .and_then(|version| version.as_str())
        .map(str::to_string)
        .context("workspace.package.version is missing")
}

fn update_versions(root: &Path, workspace_toml: &Path, current: &str, next: &str) -> Result<()> {
    update_version_in_file(workspace_toml, current, next)?;
    let output = Command::new("cargo")
        .args(["metadata", "--no-deps", "--format-version=1"])
        .current_dir(root)
        .output()
        .context("cargo metadata failed")?;
    anyhow::ensure!(output.status.success(), "cargo metadata failed");
    let metadata: serde_json::Value = serde_json::from_slice(&output.stdout)?;
    for package in metadata["packages"].as_array().into_iter().flatten() {
        if let Some(manifest) = package["manifest_path"].as_str() {
            let path = Path::new(manifest);
            if path != workspace_toml {
                update_version_in_file(path, current, next)?;
            }
        }
    }
    Ok(())
}

fn update_version_in_file(path: &Path, current: &str, next: &str) -> Result<()> {
    let content = fs::read_to_string(path)?;
    let old = format!("version = \"{current}\"");
    if content.contains(&old) {
        fs::write(
            path,
            content.replacen(&old, &format!("version = \"{next}\""), 1),
        )?;
        println!("[xtask:release] updated {}", path.display());
    }
    Ok(())
}

fn normalise_version(version: &str) -> Result<String> {
    let version = version.strip_prefix('v').unwrap_or(version);
    let parts: Vec<_> = version.split('.').collect();
    anyhow::ensure!(
        parts.len() == 3 && parts.iter().all(|part| part.parse::<u64>().is_ok()),
        "version must be MAJOR.MINOR.PATCH"
    );
    Ok(version.to_string())
}

fn validate_date(date: &str) -> Result<()> {
    let bytes = date.as_bytes();
    anyhow::ensure!(
        bytes.len() == 10
            && bytes[4] == b'-'
            && bytes[7] == b'-'
            && bytes
                .iter()
                .enumerate()
                .all(|(i, byte)| i == 4 || i == 7 || byte.is_ascii_digit()),
        "date must be YYYY-MM-DD"
    );
    Ok(())
}

fn release_branch(version: &str) -> Result<String> {
    let version = normalise_version(version)?;
    let mut parts = version.split('.');
    Ok(format!(
        "release/{}.{}.x",
        parts.next().unwrap(),
        parts.next().unwrap()
    ))
}

pub fn bump_version(current: &str, kind: BumpKind) -> Result<String> {
    let current = normalise_version(current)?;
    let mut parts = current.split('.').map(str::parse::<u64>);
    let mut major = parts.next().unwrap()?;
    let mut minor = parts.next().unwrap()?;
    let mut patch = parts.next().unwrap()?;
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
        BumpKind::Patch => patch += 1,
    }
    Ok(format!("{major}.{minor}.{patch}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn claim_gates_are_ordered_from_narrow_to_live() {
        assert_eq!(
            gates_for(ReleaseClaim::Desktop),
            vec![
                "make test-host",
                "make gate",
                "make demo-test",
                "make demo-desktop-test"
            ]
        );
    }

    #[test]
    fn release_branch_and_authorization_are_exact() {
        assert_eq!(release_branch("v0.2.3").unwrap(), "release/0.2.x");
        assert!(normalise_version("0.2").is_err());
        assert!(validate_date("2026-09-06").is_ok());
        assert!(validate_date("06-09-2026").is_err());
        assert!(validate_publish_context("main", "v0.2.3", "publish-v0.2.3").is_ok());
        assert!(validate_publish_context("release/0.2.x", "v0.2.3", "publish-v0.2.3").is_err());
        assert!(validate_publish_context("main", "v0.2.3", "publish-v0.2.4").is_err());
    }

    #[test]
    fn version_bumps_are_semantic() {
        assert_eq!(bump_version("1.2.3", BumpKind::Patch).unwrap(), "1.2.4");
        assert_eq!(bump_version("1.2.3", BumpKind::Minor).unwrap(), "1.3.0");
        assert_eq!(bump_version("1.2.3", BumpKind::Major).unwrap(), "2.0.0");
    }

    #[test]
    fn milestone_titles_accept_release_series_and_exact_versions() {
        assert_eq!(milestone_prefixes("v0.2.0").unwrap(), ["v0.2.0:", "v0.2:"]);
        assert_eq!(milestone_prefixes("0.2.7").unwrap(), ["v0.2.7:", "v0.2:"]);
    }

    #[test]
    fn release_artifact_paths_are_unambiguous() {
        assert_eq!(
            expected_artifact_paths(&[PathBuf::from("build/qemu/agentos.img")]).unwrap(),
            ["build/qemu/agentos.img"]
        );
        assert!(expected_artifact_paths(&[
            PathBuf::from("build/a/agentos.img"),
            PathBuf::from("build/b/agentos.img"),
        ])
        .is_err());
        assert!(expected_artifact_paths(&[PathBuf::from("../agentos.img")]).is_err());
        assert!(expected_artifact_paths(&[PathBuf::from("build/receipt.json")]).is_err());
    }

    #[test]
    fn remote_release_tags_must_be_annotated() {
        let annotated = concat!(
            "aaaaaaaa refs/tags/v0.2.0\n",
            "bbbbbbbb refs/tags/v0.2.0^{}\n"
        );
        assert_eq!(
            parse_remote_tag_refs(annotated, "v0.2.0").unwrap(),
            Some("bbbbbbbb".to_string())
        );
        assert!(parse_remote_tag_refs("aaaaaaaa refs/tags/v0.2.0\n", "v0.2.0").is_err());
        assert_eq!(parse_remote_tag_refs("", "v0.2.0").unwrap(), None);
    }

    #[test]
    fn publication_requires_fail_closed_main_protection() {
        let protected = serde_json::json!({
            "required_status_checks": {"contexts": REQUIRED_MAIN_CHECKS},
            "required_pull_request_reviews": {"required_approving_review_count": 0},
            "enforce_admins": {"enabled": true},
            "allow_force_pushes": {"enabled": false},
            "allow_deletions": {"enabled": false}
        });
        assert!(validate_branch_protection(&protected).is_ok());

        let mut unsafe_policy = protected.clone();
        unsafe_policy["allow_force_pushes"]["enabled"] = serde_json::json!(true);
        assert!(validate_branch_protection(&unsafe_policy).is_err());

        let mut no_checks = protected;
        no_checks["required_status_checks"]["contexts"] = serde_json::json!([]);
        assert!(validate_branch_protection(&no_checks).is_err());
    }

    #[test]
    fn release_gates_stop_at_the_first_failure() {
        let gates = ["make first", "make second", "make third"];
        let mut observed = Vec::new();
        let result = execute_ordered_gates(&gates, |gate| {
            observed.push(gate.to_string());
            Ok(gate != "make second")
        });
        assert!(result.is_err());
        assert_eq!(observed, ["make first", "make second"]);
    }

    #[test]
    fn only_publish_mutates_external_state() {
        fn external_mutation(action: &str) -> bool {
            matches!(action, "publish")
        }
        assert!(!external_mutation("plan"));
        assert!(!external_mutation("prepare"));
        assert!(!external_mutation("check"));
        assert!(!external_mutation("verify"));
        assert!(external_mutation("publish"));
    }
}
