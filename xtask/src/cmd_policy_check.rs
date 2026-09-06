use crate::PolicyCheckArgs;
use anyhow::{Context, Result};
use std::path::Path;
use std::process::Command;

const FORBIDDEN_EXTENSIONS: &[&str] = &[
    "css", "go", "html", "java", "js", "jsx", "lua", "mjs", "php", "py", "pyc", "rb", "svelte",
    "ts", "tsx", "vue", "zig",
];
const FORBIDDEN_NAMES: &[&str] = &["bun.lockb", "package.json", "yarn.lock", ".nvmrc"];

fn policy_violation(path: &str) -> bool {
    let path = Path::new(path);
    if path.components().any(|part| {
        let part = part.as_os_str().to_string_lossy();
        part == "node_modules" || part == "console" || part == "agentctl-ng"
    }) {
        return true;
    }
    if path
        .file_name()
        .and_then(|name| name.to_str())
        .map(|name| FORBIDDEN_NAMES.contains(&name))
        .unwrap_or(false)
    {
        return true;
    }
    path.extension()
        .and_then(|extension| extension.to_str())
        .map(|extension| FORBIDDEN_EXTENSIONS.contains(&extension))
        .unwrap_or(false)
}

pub fn run(_args: &PolicyCheckArgs) -> Result<()> {
    let output = Command::new("git")
        .args([
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "-z",
        ])
        .output()
        .context("failed to list tracked files")?;
    anyhow::ensure!(output.status.success(), "git ls-files failed");
    let mut violations = Vec::new();
    for raw in output
        .stdout
        .split(|byte| *byte == 0)
        .filter(|part| !part.is_empty())
    {
        let path = std::str::from_utf8(raw).context("tracked path is not UTF-8")?;
        if Path::new(path).exists() && policy_violation(path) {
            violations.push(path.to_string());
        }
    }
    violations.sort();
    if !violations.is_empty() {
        for path in &violations {
            eprintln!("POLICY_VIOLATION\t{path}");
        }
        anyhow::bail!("{} forbidden tracked file(s)", violations.len());
    }
    println!("[policy-check] repository language and UI policy passed");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_owned_forbidden_languages_and_ui_paths() {
        assert!(policy_violation("tools/helper.py"));
        assert!(policy_violation("console/main.c"));
        assert!(policy_violation("tools/agentctl-ng/main.c"));
        assert!(!policy_violation("tools/agentctl/main.c"));
    }

    #[test]
    fn vendor_files_are_not_exempt() {
        assert!(policy_violation("libvmm/dep/upstream.py"));
    }
}
