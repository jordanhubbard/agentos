//! Compare authenticated guest boot timing receipts without making a threshold claim.

use anyhow::{ensure, Context, Result};
use clap::Args;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs;
use std::path::{Path, PathBuf};

const TIMING_SCHEMA: &str = "agentos.guest_boot_timing.v2";
const COMPARISON_SCHEMA: &str = "agentos.guest_boot_timing_comparison.v2";
const AUTHENTICATED_SSH_BOUNDARY: &str =
    "host QEMU launch request to completed authenticated SSH proof";
const EXCLUDES: [&str; 3] = [
    "artifact acquisition",
    "build",
    "persistent media preparation",
];
const INCLUDES: [&str; 6] = [
    "host scheduling",
    "QEMU startup",
    "agentOS boot",
    "guest boot",
    "console provisioning",
    "SSH authentication",
];

#[derive(Args)]
pub struct GuestTimingCompareArgs {
    /// Receipt from a successful Ubuntu live authenticated-SSH boot.
    #[arg(long)]
    pub ubuntu_receipt: PathBuf,
    /// Receipt from a successful pinned Debian authenticated-SSH boot.
    #[arg(long)]
    pub debian_receipt: PathBuf,
    /// New comparison receipt. Parent directories must already exist.
    #[arg(long)]
    pub output: PathBuf,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct BootTimingReceipt {
    schema: String,
    status: String,
    qualification_status: String,
    boundary: String,
    elapsed_ms: u64,
    board: String,
    profile: String,
    agentos_revision: String,
    source_tree_clean: bool,
    agentos_image_sha256: String,
    guest_bundle_sha256: String,
    qemu_config_sha256: String,
    host_backed_virtio: bool,
    host_os: String,
    host_arch: String,
    persistent_second_boot: bool,
    serial_log: String,
    excludes: Vec<String>,
    includes: Vec<String>,
}

#[derive(Serialize)]
struct ComparisonReceipt {
    schema: &'static str,
    status: &'static str,
    boundary: &'static str,
    comparison: &'static str,
    agentos_revision: String,
    qemu_config_sha256: String,
    board: String,
    host_os: String,
    host_arch: String,
    persistent_second_boot: bool,
    ubuntu: ComparedReceipt,
    debian: ComparedReceipt,
    relative_timing: RelativeTiming,
    performance_threshold: &'static str,
}

#[derive(Serialize)]
struct ComparedReceipt {
    profile: String,
    elapsed_ms: u64,
    receipt_sha256: String,
    agentos_image_sha256: String,
    guest_bundle_sha256: String,
}

#[derive(Serialize)]
struct RelativeTiming {
    faster_profile: &'static str,
    difference_ms: u64,
}

pub fn run(args: &GuestTimingCompareArgs) -> Result<()> {
    ensure!(
        args.output != args.ubuntu_receipt && args.output != args.debian_receipt,
        "comparison output must not overwrite an input receipt"
    );
    let ubuntu = load_receipt(&args.ubuntu_receipt, "ubuntu-live")?;
    let debian = load_receipt(&args.debian_receipt, "debian")?;
    require_comparable(&ubuntu.receipt, &debian.receipt)?;

    let relative_timing = if ubuntu.receipt.elapsed_ms < debian.receipt.elapsed_ms {
        RelativeTiming {
            faster_profile: "ubuntu-live",
            difference_ms: debian.receipt.elapsed_ms - ubuntu.receipt.elapsed_ms,
        }
    } else if debian.receipt.elapsed_ms < ubuntu.receipt.elapsed_ms {
        RelativeTiming {
            faster_profile: "debian",
            difference_ms: ubuntu.receipt.elapsed_ms - debian.receipt.elapsed_ms,
        }
    } else {
        RelativeTiming {
            faster_profile: "tie",
            difference_ms: 0,
        }
    };

    let output = ComparisonReceipt {
        schema: COMPARISON_SCHEMA,
        status: "recorded",
        boundary: AUTHENTICATED_SSH_BOUNDARY,
        comparison: "same recorded host platform: Ubuntu live versus pinned Debian authenticated boot timing",
        agentos_revision: ubuntu.receipt.agentos_revision.clone(),
        qemu_config_sha256: ubuntu.receipt.qemu_config_sha256.clone(),
        board: ubuntu.receipt.board.clone(),
        host_os: ubuntu.receipt.host_os.clone(),
        host_arch: ubuntu.receipt.host_arch.clone(),
        persistent_second_boot: false,
        ubuntu: ComparedReceipt {
            profile: ubuntu.receipt.profile.clone(),
            elapsed_ms: ubuntu.receipt.elapsed_ms,
            receipt_sha256: ubuntu.sha256,
            agentos_image_sha256: ubuntu.receipt.agentos_image_sha256.clone(),
            guest_bundle_sha256: ubuntu.receipt.guest_bundle_sha256.clone(),
        },
        debian: ComparedReceipt {
            profile: debian.receipt.profile.clone(),
            elapsed_ms: debian.receipt.elapsed_ms,
            receipt_sha256: debian.sha256,
            agentos_image_sha256: debian.receipt.agentos_image_sha256.clone(),
            guest_bundle_sha256: debian.receipt.guest_bundle_sha256.clone(),
        },
        relative_timing,
        performance_threshold: "none; this receipt records measurements and does not pass or fail a performance threshold",
    };

    fs::write(&args.output, serde_json::to_vec_pretty(&output)?)
        .with_context(|| format!("write comparison receipt {}", args.output.display()))?;
    println!(
        "[xtask:guest-timing] wrote comparison receipt {}",
        args.output.display()
    );
    Ok(())
}

struct LoadedReceipt {
    receipt: BootTimingReceipt,
    sha256: String,
}

fn load_receipt(path: &Path, expected_profile: &str) -> Result<LoadedReceipt> {
    let bytes =
        fs::read(path).with_context(|| format!("read timing receipt {}", path.display()))?;
    let receipt: BootTimingReceipt = serde_json::from_slice(&bytes)
        .with_context(|| format!("malformed timing receipt {}", path.display()))?;
    validate_receipt(&receipt, expected_profile)
        .with_context(|| format!("invalid timing receipt {}", path.display()))?;
    Ok(LoadedReceipt {
        receipt,
        sha256: hex_digest(&bytes),
    })
}

fn validate_receipt(receipt: &BootTimingReceipt, expected_profile: &str) -> Result<()> {
    ensure!(
        receipt.schema == TIMING_SCHEMA,
        "unsupported timing receipt schema"
    );
    ensure!(
        receipt.status == "ssh_authenticated",
        "receipt does not prove authenticated SSH"
    );
    ensure!(
        receipt.qualification_status == "passed",
        "receipt comes from a failed or incomplete qualification"
    );
    ensure!(
        receipt.boundary == AUTHENTICATED_SSH_BOUNDARY,
        "receipt has a different timing boundary"
    );
    ensure!(receipt.elapsed_ms > 0, "receipt has a zero elapsed time");
    ensure!(
        receipt.profile == expected_profile,
        "unexpected guest profile"
    );
    ensure!(!receipt.board.trim().is_empty(), "receipt has no board");
    ensure!(
        is_git_revision(&receipt.agentos_revision),
        "receipt has no full agentOS revision"
    );
    ensure!(
        receipt.source_tree_clean,
        "receipt was generated from a dirty agentOS worktree"
    );
    ensure!(
        is_sha256(&receipt.agentos_image_sha256),
        "receipt has no agentOS image SHA-256"
    );
    ensure!(
        is_sha256(&receipt.guest_bundle_sha256),
        "receipt has no guest bundle SHA-256"
    );
    ensure!(
        is_sha256(&receipt.qemu_config_sha256),
        "receipt has no QEMU configuration SHA-256"
    );
    ensure!(
        receipt.host_backed_virtio,
        "receipt does not require host-backed VirtIO proof"
    );
    ensure!(!receipt.host_os.trim().is_empty(), "receipt has no host OS");
    ensure!(
        !receipt.host_arch.trim().is_empty(),
        "receipt has no host architecture"
    );
    ensure!(
        !receipt.serial_log.trim().is_empty(),
        "receipt has no serial log path"
    );
    ensure!(
        !receipt.persistent_second_boot,
        "second persistent boots must be compared separately"
    );
    ensure!(
        receipt.excludes == EXCLUDES,
        "receipt has a different timing exclusion set"
    );
    ensure!(
        receipt.includes == INCLUDES,
        "receipt has a different timing inclusion set"
    );
    Ok(())
}

fn require_comparable(ubuntu: &BootTimingReceipt, debian: &BootTimingReceipt) -> Result<()> {
    ensure!(
        ubuntu.agentos_revision == debian.agentos_revision,
        "receipts use different agentOS revisions"
    );
    ensure!(
        ubuntu.board == debian.board,
        "receipts use different boards"
    );
    ensure!(
        ubuntu.qemu_config_sha256 == debian.qemu_config_sha256,
        "receipts use different QEMU configurations"
    );
    ensure!(
        ubuntu.host_os == debian.host_os && ubuntu.host_arch == debian.host_arch,
        "receipts use different recorded host platforms"
    );
    ensure!(
        ubuntu.persistent_second_boot == debian.persistent_second_boot,
        "receipts use different persistent-boot phases"
    );
    Ok(())
}

fn is_git_revision(revision: &str) -> bool {
    revision.len() == 40 && revision.bytes().all(|byte| byte.is_ascii_hexdigit())
}

fn is_sha256(digest: &str) -> bool {
    digest.len() == 64 && digest.bytes().all(|byte| byte.is_ascii_hexdigit())
}

fn hex_digest(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn receipt(profile: &str) -> serde_json::Value {
        json!({
            "schema": TIMING_SCHEMA,
            "status": "ssh_authenticated",
            "qualification_status": "passed",
            "boundary": AUTHENTICATED_SSH_BOUNDARY,
            "elapsed_ms": 1000,
            "board": "qemu_virt_aarch64",
            "profile": profile,
            "agentos_revision": "0123456789abcdef0123456789abcdef01234567",
            "source_tree_clean": true,
            "agentos_image_sha256": "a".repeat(64),
            "guest_bundle_sha256": "b".repeat(64),
            "qemu_config_sha256": "c".repeat(64),
            "host_backed_virtio": true,
            "host_os": "macos",
            "host_arch": "aarch64",
            "persistent_second_boot": false,
            "serial_log": "/evidence/serial.log",
            "excludes": EXCLUDES,
            "includes": INCLUDES,
        })
    }

    fn write_receipt(path: &Path, receipt: serde_json::Value) {
        fs::write(path, serde_json::to_vec_pretty(&receipt).unwrap()).unwrap();
    }

    #[test]
    fn comparison_writes_measurements_without_a_threshold() {
        let directory = tempfile::tempdir().unwrap();
        let ubuntu_path = directory.path().join("ubuntu.json");
        let debian_path = directory.path().join("debian.json");
        let output_path = directory.path().join("comparison.json");
        write_receipt(&ubuntu_path, receipt("ubuntu-live"));
        let mut debian = receipt("debian");
        debian["elapsed_ms"] = json!(1300);
        write_receipt(&debian_path, debian);

        run(&GuestTimingCompareArgs {
            ubuntu_receipt: ubuntu_path,
            debian_receipt: debian_path,
            output: output_path.clone(),
        })
        .unwrap();

        let output: serde_json::Value =
            serde_json::from_slice(&fs::read(output_path).unwrap()).unwrap();
        assert_eq!(output["schema"], COMPARISON_SCHEMA);
        assert_eq!(output["relative_timing"]["faster_profile"], "ubuntu-live");
        assert_eq!(output["relative_timing"]["difference_ms"], 300);
        assert_eq!(output["qemu_config_sha256"], "c".repeat(64));
        assert_eq!(
            output["performance_threshold"],
            "none; this receipt records measurements and does not pass or fail a performance threshold"
        );
    }

    #[test]
    fn comparison_rejects_different_revisions() {
        let directory = tempfile::tempdir().unwrap();
        let ubuntu_path = directory.path().join("ubuntu.json");
        let debian_path = directory.path().join("debian.json");
        write_receipt(&ubuntu_path, receipt("ubuntu-live"));
        let mut debian = receipt("debian");
        debian["agentos_revision"] = json!("fedcba9876543210fedcba9876543210fedcba98");
        write_receipt(&debian_path, debian);

        let result = run(&GuestTimingCompareArgs {
            ubuntu_receipt: ubuntu_path,
            debian_receipt: debian_path,
            output: directory.path().join("comparison.json"),
        });
        assert!(result
            .unwrap_err()
            .to_string()
            .contains("different agentOS revisions"));
    }

    #[test]
    fn comparison_rejects_different_host_platforms() {
        let directory = tempfile::tempdir().unwrap();
        let ubuntu_path = directory.path().join("ubuntu.json");
        let debian_path = directory.path().join("debian.json");
        write_receipt(&ubuntu_path, receipt("ubuntu-live"));
        let mut debian = receipt("debian");
        debian["host_arch"] = json!("x86_64");
        write_receipt(&debian_path, debian);

        let result = run(&GuestTimingCompareArgs {
            ubuntu_receipt: ubuntu_path,
            debian_receipt: debian_path,
            output: directory.path().join("comparison.json"),
        });
        assert!(result
            .unwrap_err()
            .to_string()
            .contains("different recorded host platforms"));
    }

    #[test]
    fn comparison_rejects_dirty_source_or_different_qemu_configuration() {
        let directory = tempfile::tempdir().unwrap();
        let ubuntu_path = directory.path().join("ubuntu.json");
        let debian_path = directory.path().join("debian.json");
        let mut ubuntu = receipt("ubuntu-live");
        ubuntu["source_tree_clean"] = json!(false);
        write_receipt(&ubuntu_path, ubuntu);
        write_receipt(&debian_path, receipt("debian"));

        let result = run(&GuestTimingCompareArgs {
            ubuntu_receipt: ubuntu_path.clone(),
            debian_receipt: debian_path.clone(),
            output: directory.path().join("comparison.json"),
        });
        assert!(format!("{:#}", result.unwrap_err()).contains("dirty agentOS worktree"));

        write_receipt(&ubuntu_path, receipt("ubuntu-live"));
        let mut debian = receipt("debian");
        debian["qemu_config_sha256"] = json!("d".repeat(64));
        write_receipt(&debian_path, debian);
        let result = run(&GuestTimingCompareArgs {
            ubuntu_receipt: ubuntu_path,
            debian_receipt: debian_path,
            output: directory.path().join("comparison.json"),
        });
        assert!(result
            .unwrap_err()
            .to_string()
            .contains("different QEMU configurations"));
    }

    #[test]
    fn comparison_rejects_receipts_without_host_backed_virtio() {
        let directory = tempfile::tempdir().unwrap();
        let ubuntu_path = directory.path().join("ubuntu.json");
        let debian_path = directory.path().join("debian.json");
        let mut ubuntu = receipt("ubuntu-live");
        ubuntu["host_backed_virtio"] = json!(false);
        write_receipt(&ubuntu_path, ubuntu);
        write_receipt(&debian_path, receipt("debian"));

        let result = run(&GuestTimingCompareArgs {
            ubuntu_receipt: ubuntu_path,
            debian_receipt: debian_path,
            output: directory.path().join("comparison.json"),
        });
        assert!(format!("{:#}", result.unwrap_err()).contains("host-backed VirtIO proof"));
    }

    #[test]
    fn comparison_rejects_malformed_or_wrong_profile_receipts() {
        let directory = tempfile::tempdir().unwrap();
        let ubuntu_path = directory.path().join("ubuntu.json");
        let debian_path = directory.path().join("debian.json");
        write_receipt(&ubuntu_path, json!({"schema": TIMING_SCHEMA}));
        write_receipt(&debian_path, receipt("ubuntu-live"));

        let result = run(&GuestTimingCompareArgs {
            ubuntu_receipt: ubuntu_path,
            debian_receipt: debian_path,
            output: directory.path().join("comparison.json"),
        });
        assert!(result
            .unwrap_err()
            .to_string()
            .contains("malformed timing receipt"));
    }

    #[test]
    fn comparison_rejects_missing_wrong_profile_and_input_overwrite() {
        let directory = tempfile::tempdir().unwrap();
        let ubuntu_path = directory.path().join("ubuntu.json");
        let debian_path = directory.path().join("debian.json");
        write_receipt(&ubuntu_path, receipt("ubuntu-live"));
        write_receipt(&debian_path, receipt("ubuntu-live"));

        let result = run(&GuestTimingCompareArgs {
            ubuntu_receipt: ubuntu_path.clone(),
            debian_receipt: debian_path.clone(),
            output: directory.path().join("comparison.json"),
        });
        assert!(format!("{:#}", result.unwrap_err()).contains("unexpected guest profile"));

        let result = run(&GuestTimingCompareArgs {
            ubuntu_receipt: directory.path().join("missing.json"),
            debian_receipt: debian_path,
            output: directory.path().join("comparison.json"),
        });
        assert!(result
            .unwrap_err()
            .to_string()
            .contains("read timing receipt"));

        let result = run(&GuestTimingCompareArgs {
            ubuntu_receipt: ubuntu_path.clone(),
            debian_receipt: ubuntu_path.clone(),
            output: ubuntu_path,
        });
        assert!(result
            .unwrap_err()
            .to_string()
            .contains("must not overwrite an input receipt"));
    }
}
