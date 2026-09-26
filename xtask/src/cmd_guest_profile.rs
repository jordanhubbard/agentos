//! Compile bounded guest profile TOML into the target manifest wire format.

use anyhow::{ensure, Context, Result};
use clap::Args;
use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::io::Read;
use std::path::{Component, Path, PathBuf};
use std::process::Command;

const MAGIC: u64 = 0x0046_5250_4753_4f41;
const VERSION: u16 = 2;
const MANIFEST_SIZE: usize = 640;
const MAX_INHERITANCE_DEPTH: usize = 8;
const MAX_RECIPE_STEPS: usize = 64;
const MAX_VCPUS: u32 = 8;
const RAM_MIN: u64 = 0x20_0000;
const RAM_MAX: u64 = 0x2_0000_0000;
const RAM_ALIGN: u64 = 0x20_0000;
const MAX_SELECTED_PLACEMENTS: usize = 4;
const CPU_FEATURES_VERSION: u8 = 1;

#[derive(Args)]
pub struct GuestProfileArgs {
    /// Directory containing profile TOML files.
    #[arg(long, default_value = "guest-profiles")]
    pub root: PathBuf,
    /// Profile path, relative to --root.
    #[arg(long)]
    pub profile: Option<PathBuf>,
    /// Resolve a profile alias to its root-relative TOML path.
    #[arg(long)]
    pub resolve_alias: Option<String>,
    /// Placement name to compile into the target manifest.
    #[arg(long, default_value = "default")]
    pub placement: String,
    /// Binary target manifest output. Required when compiling one profile.
    #[arg(long)]
    pub output: Option<PathBuf>,
    /// Resolve and validate every TOML profile without emitting manifests.
    #[arg(long)]
    pub check_all: bool,
    /// Hash every runtime artifact at its cache path before emitting.
    #[arg(long)]
    pub verify_artifacts: bool,
    /// Base directory for relative artifact cache paths.
    #[arg(long, default_value = ".")]
    pub repo_root: PathBuf,
    /// Prepare a canonical build bundle instead of emitting only a manifest.
    #[arg(long)]
    pub prepare_dir: Option<PathBuf>,
    /// Print the selected placement's RAM size in bytes without emitting.
    #[arg(long)]
    pub print_ram_size: bool,
    /// Print the profile's target control type without emitting.
    #[arg(long)]
    pub print_control_type: bool,
    /// Prepare verified x86 artifacts for an explicitly selected VMM build slot.
    #[arg(long, value_enum, conflicts_with_all = ["resolve_alias", "check_all", "output", "prepare_dir", "print_ram_size", "print_control_type"])]
    pub prepare_x86_slot: Option<X86BuildSlot>,
}

#[derive(Clone, Copy, Debug, clap::ValueEnum)]
pub enum X86BuildSlot {
    Primary,
    Secondary,
}

impl X86BuildSlot {
    fn owner(self) -> u32 {
        match self {
            Self::Primary => 0,
            Self::Secondary => 1,
        }
    }

    fn name(self) -> &'static str {
        match self {
            Self::Primary => "primary",
            Self::Secondary => "secondary",
        }
    }
}

#[derive(Clone, Copy, Debug, Deserialize, PartialEq)]
#[serde(rename_all = "kebab-case")]
enum Status {
    Abstract,
    Planned,
    Runtime,
}

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct Profile {
    schema: Option<u16>,
    id: Option<String>,
    status: Option<Status>,
    #[serde(default)]
    aliases: Vec<String>,
    target: Option<Target>,
    boot: Option<Boot>,
    #[serde(default)]
    artifacts: BTreeMap<String, Artifact>,
    #[serde(default)]
    placements: BTreeMap<String, Placement>,
    host: Option<Host>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Target {
    architecture: Option<String>,
    boot_protocol: Option<String>,
    kernel_format: Option<String>,
    control_type: Option<u32>,
    guest_id: Option<u32>,
    vcpus: Option<u32>,
    cpu_features: Option<CpuFeatures>,
    devices: Option<Vec<String>>,
    network_client: Option<u16>,
    block_media: Option<u16>,
    autostart: Option<bool>,
    entry_from_image: Option<bool>,
}

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct CpuFeatures {
    version: Option<u8>,
    #[serde(default)]
    required: Vec<String>,
    #[serde(default)]
    prohibited: Vec<String>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Boot {
    command_line: Option<String>,
    media_initrd_path: Option<String>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Artifact {
    source: Option<String>,
    cache: Option<String>,
    sha256: Option<String>,
    max_bytes: Option<u64>,
}

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct Placement {
    guest_gpa_base: Option<u64>,
    vmm_hva_base: Option<u64>,
    ram_size: Option<u64>,
    kernel_load_address: Option<u64>,
    kernel_entry_address: Option<u64>,
    dtb_load_address: Option<u64>,
    initrd_load_address: Option<u64>,
    kernel_sha256: Option<String>,
    dtb_sha256: Option<String>,
    initrd_sha256: Option<String>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Host {
    seed: Option<SeedPlan>,
    qemu: Option<Qemu>,
    console: Option<HostConsole>,
    desktop: Option<HostDesktop>,
    build: Option<HostBuild>,
    #[serde(default)]
    acquire: Vec<RecipeStep>,
    #[serde(default)]
    provision: Vec<RecipeStep>,
    #[serde(default)]
    test: Vec<RecipeStep>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct SeedPlan {
    pub(crate) adapter: String,
    pub(crate) root_ext4: String,
    pub(crate) disk_raw: String,
    pub(crate) partition_offset: u64,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct HostDesktop {
    adapter: String,
    local_port: u16,
    guest_port: Option<u16>,
    guest_socket: Option<String>,
    provision_timeout_secs: u64,
    frame_timeout_secs: u64,
    io_timeout_secs: u64,
    provision_script: String,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct HostBuild {
    adapter: String,
    template: Option<String>,
    base: Option<String>,
    bootargs: Option<String>,
    initrd_total_bytes: Option<u64>,
    media_initrd_cache: Option<String>,
    acquire_dir: String,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct HostConsole {
    adapter: String,
    #[serde(default)]
    success: Vec<String>,
    #[serde(default)]
    require: Vec<String>,
    #[serde(default)]
    reject: Vec<String>,
    #[serde(default)]
    interaction: Vec<ConsoleInteraction>,
    probe_line: Option<String>,
    probe_marker: Option<String>,
    probe_timeout_secs: Option<u64>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct ConsoleInteraction {
    #[serde(default)]
    when: Vec<String>,
    send: String,
    #[serde(default = "one_console_fire")]
    max_fires: u8,
    after_secs: Option<u64>,
}

fn one_console_fire() -> u8 {
    1
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Qemu {
    board: String,
    machine: String,
    memory: String,
    #[serde(default)]
    media: Vec<QemuMedia>,
    ssh: Option<QemuSsh>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct QemuMedia {
    path: String,
    drive_id: String,
    bus: u8,
    #[serde(default)]
    writable: bool,
    #[serde(default)]
    override_env: Vec<String>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct QemuSsh {
    account: String,
    host_port: u16,
    guest_address: Option<String>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct RecipeStep {
    pub(crate) action: String,
    #[serde(default)]
    pub(crate) args: BTreeMap<String, String>,
}

#[derive(Clone, Debug)]
pub(crate) struct HostProfilePlan {
    pub(crate) seed: Option<SeedPlan>,
    pub(crate) path: PathBuf,
    pub(crate) id: String,
    pub(crate) architecture: String,
    pub(crate) control_type: u32,
    pub(crate) guest_id: u32,
    pub(crate) devices: Vec<String>,
    pub(crate) media_initrd_path: Option<String>,
    pub(crate) qemu: Option<QemuPlan>,
    pub(crate) console: ConsolePlan,
    pub(crate) desktop: Option<DesktopPlan>,
    pub(crate) provision: Vec<RecipeStep>,
    pub(crate) test: Vec<RecipeStep>,
}

#[derive(Clone, Debug)]
pub(crate) struct DesktopPlan {
    pub(crate) local_port: u16,
    pub(crate) guest_port: Option<u16>,
    pub(crate) guest_socket: Option<String>,
    pub(crate) provision_timeout_secs: u64,
    pub(crate) frame_timeout_secs: u64,
    pub(crate) io_timeout_secs: u64,
    pub(crate) provision_script: String,
}

#[derive(Clone, Debug)]
pub(crate) struct ConsolePlan {
    pub(crate) success: Vec<String>,
    pub(crate) require: Vec<String>,
    pub(crate) reject: Vec<String>,
    pub(crate) interaction: Vec<ConsoleInteractionPlan>,
    pub(crate) probe_line: Option<String>,
    pub(crate) probe_marker: Option<String>,
    pub(crate) probe_timeout_secs: u64,
}

#[derive(Clone, Debug)]
pub(crate) struct ConsoleInteractionPlan {
    pub(crate) when: Vec<String>,
    pub(crate) send: String,
    pub(crate) max_fires: u8,
    pub(crate) after_secs: u64,
}

impl Default for ConsolePlan {
    fn default() -> Self {
        Self {
            success: Vec::new(),
            require: Vec::new(),
            reject: Vec::new(),
            interaction: Vec::new(),
            probe_line: None,
            probe_marker: None,
            probe_timeout_secs: 20,
        }
    }
}

#[derive(Clone, Debug)]
pub(crate) struct QemuPlan {
    pub(crate) board: String,
    pub(crate) machine: String,
    pub(crate) memory: String,
    pub(crate) media: Vec<QemuMediaPlan>,
    pub(crate) ssh: Option<QemuSshPlan>,
}

#[derive(Clone, Debug)]
pub(crate) struct QemuMediaPlan {
    pub(crate) path: String,
    pub(crate) drive_id: String,
    pub(crate) bus: u8,
    pub(crate) writable: bool,
    /// Set only by the managed two-boot proof, never by profile input.
    pub(crate) managed_persistent: bool,
    pub(crate) override_env: Vec<String>,
}

#[derive(Clone, Debug)]
pub(crate) struct QemuSshPlan {
    pub(crate) account: String,
    pub(crate) host_port: u16,
    pub(crate) guest_address: Option<String>,
}

const HOST_ACTIONS: &[&str] = &[
    "download",
    "stage-url",
    "download-tar-member",
    "verify-sha256",
    "extract-iso-file",
    "extract-ufs-file",
    "extract-arm64-linux-image",
    "extract-arm64-elf-image",
    "build-initramfs-file",
    "build-static-linux-elf",
    "append-initramfs-file",
    "filter-initramfs-modules",
    "convert-qcow2-raw",
    "extract-gpt-partition",
    "extract-ext4-file",
    "install-gpt-ext4-file",
    "normalize-arm64-linux-image",
    "decompress-gzip",
    "copy",
    "build-initramfs",
    "build-linux-probe-initramfs",
    "wait-console",
    "send-console",
    "wait-ssh",
    "ssh-check",
    "run-ssh",
    "assert-console",
    "assert-virtio",
    "assert-frame-pixels",
    "assert-ssh-output",
];

pub(crate) fn acquire_recipe(root: &Path, path: &Path) -> Result<(String, Vec<RecipeStep>)> {
    let (profile, _) = resolve(root, path, &mut Vec::new())?;
    validate(&profile, None)?;
    ensure!(
        profile.status == Some(Status::Runtime),
        "only status=runtime profiles can acquire boot artifacts"
    );
    let id = profile.id.context("id is required")?;
    let steps = profile.host.map(|host| host.acquire).unwrap_or_default();
    for step in &steps {
        ensure!(
            matches!(
                step.action.as_str(),
                "stage-url"
                    | "download-tar-member"
                    | "extract-iso-file"
                    | "extract-arm64-linux-image"
                    | "extract-arm64-elf-image"
                    | "build-initramfs-file"
                    | "build-static-linux-elf"
                    | "append-initramfs-file"
                    | "filter-initramfs-modules"
                    | "convert-qcow2-raw"
                    | "extract-gpt-partition"
                    | "extract-ext4-file"
                    | "install-gpt-ext4-file"
                    | "normalize-arm64-linux-image"
                    | "build-linux-probe-initramfs"
            ),
            "runtime host.acquire action {:?} has no bounded executor",
            step.action
        );
    }
    Ok((id, steps))
}

pub(crate) fn acquire_output_dir(root: &Path, path: &Path) -> Result<PathBuf> {
    let (profile, _) = resolve(root, path, &mut Vec::new())?;
    validate(&profile, None)?;
    ensure!(
        profile.status == Some(Status::Runtime),
        "only status=runtime profiles can acquire boot artifacts"
    );
    let value = &profile
        .host
        .as_ref()
        .and_then(|host| host.build.as_ref())
        .context("runtime profile requires host.build metadata")?
        .acquire_dir;
    validate_repo_relative(value, "host.build.acquire_dir")?;
    Ok(PathBuf::from(value))
}

pub(crate) fn resolve_alias(root: &Path, alias: &str) -> Result<PathBuf> {
    ensure!(
        valid_alias(alias),
        "guest profile alias must be 1..63 lowercase ASCII letters, digits, or hyphens"
    );
    let mut matched = None;
    for path in profile_files(root)? {
        let (profile, _) = resolve(root, &path, &mut Vec::new())?;
        if profile.aliases.iter().any(|candidate| candidate == alias) {
            ensure!(
                matched.is_none(),
                "guest profile alias {alias:?} is ambiguous"
            );
            matched = Some(path);
        }
    }
    matched.with_context(|| format!("unknown guest profile alias {alias:?}"))
}

#[cfg(test)]
fn validate_x86_boot_profile(profile: &Profile) -> Result<()> {
    validate_x86_slot_profile(profile, X86BuildSlot::Primary)
}

fn validate_x86_slot_profile(profile: &Profile, slot: X86BuildSlot) -> Result<()> {
    validate(profile, Some("default"))?;
    ensure!(
        profile.status == Some(Status::Runtime),
        "x86 boot requires a runtime profile"
    );
    let target = profile.target.as_ref().unwrap();
    ensure!(
        target.architecture.as_deref() == Some("x86-64")
            && target.boot_protocol.as_deref() == Some("uefi")
            && target.kernel_format.as_deref() == Some("uefi"),
        "x86 boot requires an x86-64 UEFI kernel profile"
    );
    ensure!(
        matches!(target.vcpus, Some(1 | 2))
            && target.guest_id == Some(slot.owner())
            && target.control_type == Some(slot.owner() + 1)
            && target.autostart == Some(true),
        "x86 boot profile must match the selected build slot and have one or two provisioned vCPUs"
    );
    if let Some(features) = &target.cpu_features {
        // Keep admission aligned with the synthetic target CPUID model.
        // These are exposure requirements, not instruction-trapping policy.
        let exposed = ["fp", "simd"];
        ensure!(
            features.required.iter().all(|f| exposed.contains(&f.as_str()))
                && features.prohibited.iter().all(|f| !exposed.contains(&f.as_str())),
            "x86 CPU profile exposes fixed fp/simd; requested requirements or prohibitions are unavailable"
        );
    }
    let devices = target.devices.as_ref().unwrap();
    ensure!(
        (3..=5).contains(&devices.len())
            && devices
                .iter()
                .all(|device| ["net", "block", "console", "gpu", "input"].contains(&device.as_str()))
            && ["net", "block", "console"]
                .iter()
                .all(|d| devices.iter().any(|v| v == d))
            && target.network_client == Some(slot.owner() as u16)
            && target.block_media == Some(slot.owner() as u16),
        "x86 boot requires canonical net, block and console devices, with optional gpu/input, owned by the selected slot"
    );
    ensure!(
        !profile.artifacts.contains_key("dtb")
            && profile.artifacts.contains_key("initrd")
            && profile.boot.as_ref().unwrap().media_initrd_path.is_none(),
        "x86 firmware boot requires a direct initrd and no DTB"
    );
    let placement = &profile.placements["default"];
    let ram = placement.ram_size.unwrap();
    ensure!(
        placement.vmm_hva_base == Some(0x80000000) && placement.kernel_entry_address.is_none(),
        "x86 firmware boot requires its fixed RAM mapping and firmware-selected kernel entry"
    );
    ensure!(
        placement.guest_gpa_base == Some(0)
            && (0x02000000..=0x80000000).contains(&ram)
            && ram % 0x200000 == 0,
        "x86 firmware RAM must start at zero and be 32 MiB..2 GiB in 2 MiB units"
    );
    ensure!(
        profile
            .host
            .as_ref()
            .and_then(|h| h.build.as_ref())
            .is_some_and(|b| b.adapter == "uefi-artifacts"),
        "x86 boot requires the uefi-artifacts acquisition adapter"
    );
    Ok(())
}

pub(crate) fn prepare_x86_boot_profile(repo: &Path, path: &Path) -> Result<Vec<String>> {
    let root = repo.join("guest-profiles");
    prepare_x86_slot_profile(repo, &root, path, X86BuildSlot::Primary)
}

fn prepare_x86_slot_profile(
    repo: &Path,
    root: &Path,
    path: &Path,
    slot: X86BuildSlot,
) -> Result<Vec<String>> {
    let absolute_repo = fs::canonicalize(repo).context("resolve x86 artifact repository")?;
    let repo = absolute_repo.as_path();
    let (profile, canonical) = resolve(root, path, &mut Vec::new())?;
    validate_x86_slot_profile(&profile, slot)?;
    if let Some(selected) = std::env::var_os("X86_VMM_SLOT") {
        ensure!(
            selected == slot.name(),
            "X86_VMM_SLOT conflicts with selected build slot"
        );
    }
    for name in [
        "X86_BOOT_KERNEL",
        "X86_BOOT_KERNEL_SHA256",
        "X86_BOOT_INITRD",
        "X86_BOOT_INITRD_SHA256",
        "X86_BOOT_CMDLINE_FILE",
        "X86_BOOT_CMDLINE_SHA256",
        "X86_BOOT_RAM_BYTES",
        "X86_BOOT_PROFILE_BIN",
        "X86_BOOT_PROFILE_SHA256",
    ] {
        ensure!(
            std::env::var_os(name).is_none(),
            "{name} conflicts with x86 boot profile selection"
        );
    }
    crate::cmd_fetch_guest::run(&crate::FetchGuestArgs {
        profile: path.to_path_buf(),
        profile_root: root.to_path_buf(),
        output_dir: None,
    })?;
    verify_artifacts(&profile, "default", repo)?;
    let directory = repo.join("_build/tmp/x86-boot-profile").join(slot.name());
    fs::create_dir_all(&directory)?;
    let mut command_line = profile
        .boot
        .as_ref()
        .unwrap()
        .command_line
        .as_ref()
        .unwrap()
        .as_bytes()
        .to_vec();
    command_line.push(0);
    let command_path = directory.join("cmdline.bin");
    fs::write(&command_path, &command_line)?;
    let manifest = compile(&profile, &canonical, "default")?;
    let manifest_path = directory.join("profile.bin");
    fs::write(&manifest_path, &manifest)?;
    let mut args = vec![
        format!("X86_VMM_SLOT={}", slot.name()),
        format!("X86_BOOT_PROFILE_BIN={}", manifest_path.display()),
        format!("X86_BOOT_PROFILE_SHA256={:x}", Sha256::digest(&manifest)),
        format!(
            "X86_BOOT_RAM_BYTES={:#x}u",
            profile.placements["default"].ram_size.unwrap()
        ),
        format!("X86_BOOT_CMDLINE_FILE={}", command_path.display()),
        format!(
            "X86_BOOT_CMDLINE_SHA256={:x}",
            Sha256::digest(&command_line)
        ),
    ];
    for (name, variable) in [("kernel", "KERNEL"), ("initrd", "INITRD")] {
        let artifact = &profile.artifacts[name];
        let hash = artifact_hash(artifact, &profile.placements["default"], name)?;
        let hex: String = hash.iter().map(|b| format!("{b:02x}")).collect();
        args.push(format!(
            "X86_BOOT_{variable}={}",
            artifact_path(&profile, name, repo)?.display()
        ));
        args.push(format!("X86_BOOT_{variable}_SHA256={hex}"));
    }
    println!(
        "[guest-profile] verified x86 boot profile {}",
        profile.id.as_deref().unwrap()
    );
    Ok(args)
}

pub(crate) fn host_profile_plan(root: &Path, path: &Path) -> Result<HostProfilePlan> {
    let (profile, _) = resolve(root, path, &mut Vec::new())?;
    validate(&profile, None)?;
    ensure!(
        profile.status == Some(Status::Runtime),
        "only status=runtime profiles can be executed by host tooling"
    );
    let target = profile
        .target
        .as_ref()
        .context("target table is required")?;
    let host = profile.host.as_ref();
    let qemu = host
        .and_then(|value| value.qemu.as_ref())
        .map(|value| QemuPlan {
            board: value.board.clone(),
            machine: value.machine.clone(),
            memory: value.memory.clone(),
            media: value
                .media
                .iter()
                .map(|media| QemuMediaPlan {
                    path: media.path.clone(),
                    drive_id: media.drive_id.clone(),
                    bus: media.bus,
                    writable: media.writable,
                    managed_persistent: false,
                    override_env: media.override_env.clone(),
                })
                .collect(),
            ssh: value.ssh.as_ref().map(|ssh| QemuSshPlan {
                account: ssh.account.clone(),
                host_port: ssh.host_port,
                guest_address: ssh.guest_address.clone(),
            }),
        });
    ensure!(
        !host.is_some_and(|h| h.test.iter().any(|s| s.action == "assert-frame-pixels"))
            || target
                .devices
                .as_ref()
                .is_some_and(|ds| ds.iter().any(|d| d == "gpu")),
        "frame pixel assertions require target.devices gpu"
    );
    Ok(HostProfilePlan {
        seed: host.and_then(|value| value.seed.clone()),
        path: path.to_path_buf(),
        id: profile.id.clone().context("id is required")?,
        architecture: target
            .architecture
            .clone()
            .context("target.architecture is required")?,
        control_type: target
            .control_type
            .context("target.control_type is required")?,
        guest_id: target.guest_id.context("target.guest_id is required")?,
        devices: target
            .devices
            .clone()
            .context("target.devices is required")?,
        media_initrd_path: profile
            .boot
            .as_ref()
            .and_then(|boot| boot.media_initrd_path.clone()),
        qemu,
        console: host
            .and_then(|value| value.console.as_ref())
            .map(|console| ConsolePlan {
                success: console.success.clone(),
                require: console.require.clone(),
                reject: console.reject.clone(),
                interaction: console
                    .interaction
                    .iter()
                    .map(|interaction| ConsoleInteractionPlan {
                        when: interaction.when.clone(),
                        send: interaction.send.clone(),
                        max_fires: interaction.max_fires,
                        after_secs: interaction.after_secs.unwrap_or(0),
                    })
                    .collect(),
                probe_line: console.probe_line.clone(),
                probe_marker: console.probe_marker.clone(),
                probe_timeout_secs: console.probe_timeout_secs.unwrap_or(20),
            })
            .unwrap_or_default(),
        desktop: host
            .and_then(|value| value.desktop.as_ref())
            .map(|desktop| DesktopPlan {
                local_port: desktop.local_port,
                guest_port: desktop.guest_port,
                guest_socket: desktop.guest_socket.clone(),
                provision_timeout_secs: desktop.provision_timeout_secs,
                frame_timeout_secs: desktop.frame_timeout_secs,
                io_timeout_secs: desktop.io_timeout_secs,
                provision_script: desktop.provision_script.clone(),
            }),
        provision: host
            .map(|value| value.provision.clone())
            .unwrap_or_default(),
        test: host.map(|value| value.test.clone()).unwrap_or_default(),
    })
}

pub fn run(args: &GuestProfileArgs) -> Result<()> {
    ensure!(
        args.root.is_dir(),
        "profile root does not exist: {}",
        args.root.display()
    );
    if let Some(alias) = &args.resolve_alias {
        ensure!(
            !args.check_all
                && args.profile.is_none()
                && args.output.is_none()
                && args.prepare_dir.is_none()
                && !args.verify_artifacts
                && !args.print_ram_size
                && !args.print_control_type,
            "--resolve-alias cannot be combined with compilation options"
        );
        let path = resolve_alias(&args.root, alias)?;
        println!(
            "{}",
            path.strip_prefix(&args.root).unwrap_or(&path).display()
        );
        return Ok(());
    }
    if args.check_all {
        ensure!(
            args.profile.is_none(),
            "--check-all and --profile are mutually exclusive"
        );
        ensure!(args.output.is_none(), "--check-all does not emit --output");
        ensure!(
            args.prepare_dir.is_none(),
            "--check-all does not prepare a build bundle"
        );
        ensure!(
            !args.verify_artifacts,
            "--check-all cannot verify artifacts for mutually exclusive profiles"
        );
        ensure!(
            !args.print_ram_size,
            "--check-all cannot print one placement's RAM size"
        );
        ensure!(
            !args.print_control_type,
            "--check-all cannot print one profile's control type"
        );
        let files = profile_files(&args.root)?;
        ensure!(
            !files.is_empty(),
            "no TOML profiles under {}",
            args.root.display()
        );
        let mut aliases = BTreeMap::new();
        for path in &files {
            let (profile, _) = resolve(&args.root, path, &mut Vec::new())?;
            validate(&profile, None)
                .with_context(|| format!("invalid profile {}", path.display()))?;
            for alias in &profile.aliases {
                ensure!(
                    aliases.insert(alias.clone(), path.clone()).is_none(),
                    "duplicate guest profile alias {alias:?}"
                );
            }
        }
        println!("[guest-profile] validated {} profiles", files.len());
        return Ok(());
    }

    let profile_path = args
        .profile
        .as_ref()
        .context("--profile is required unless --check-all is used")?;
    if let Some(slot) = args.prepare_x86_slot {
        ensure!(
            args.placement == "default",
            "x86 boot requires default placement"
        );
        let build_args = prepare_x86_slot_profile(&args.repo_root, &args.root, profile_path, slot)?;
        // Structured output preserves paths with spaces; these are argv entries,
        // not shell code to evaluate.
        println!("{}", serde_json::to_string(&build_args)?);
        return Ok(());
    }
    if args.print_ram_size || args.print_control_type {
        ensure!(
            !(args.print_ram_size && args.print_control_type)
                && args.output.is_none()
                && args.prepare_dir.is_none()
                && !args.verify_artifacts,
            "profile field queries cannot be combined with each other or emission options"
        );
        let (profile, _) = resolve(&args.root, profile_path, &mut Vec::new())?;
        validate(
            &profile,
            args.print_ram_size.then_some(args.placement.as_str()),
        )?;
        ensure!(
            profile.status == Some(Status::Runtime),
            "only status=runtime profiles have an executable build plan"
        );
        if args.print_ram_size {
            println!(
                "{}",
                profile.placements[&args.placement]
                    .ram_size
                    .context("placement.ram_size is required")?
            );
        } else {
            println!(
                "{}",
                profile
                    .target
                    .as_ref()
                    .and_then(|target| target.control_type)
                    .context("target.control_type is required")?
            );
        }
        return Ok(());
    }
    if let Some(prepare_dir) = &args.prepare_dir {
        ensure!(
            args.output.is_none(),
            "--prepare-dir and --output are mutually exclusive"
        );
        return prepare_bundle(
            &args.root,
            profile_path,
            &args.placement,
            &args.repo_root,
            prepare_dir,
        );
    }
    let output = args
        .output
        .as_ref()
        .context("--output is required when compiling a profile")?;
    let (profile, canonical) = resolve(&args.root, profile_path, &mut Vec::new())?;
    validate(&profile, Some(&args.placement))?;
    ensure!(
        profile.status == Some(Status::Runtime),
        "only status=runtime profiles can enter a target image"
    );
    if args.verify_artifacts {
        verify_artifacts(&profile, &args.placement, &args.repo_root)?;
    }
    let bytes = compile(&profile, &canonical, &args.placement)?;
    if let Some(parent) = output.parent() {
        fs::create_dir_all(parent).with_context(|| format!("creating {}", parent.display()))?;
    }
    fs::write(output, bytes).with_context(|| format!("writing {}", output.display()))?;
    println!(
        "[guest-profile] wrote {} bytes to {}",
        MANIFEST_SIZE,
        output.display()
    );
    Ok(())
}

fn prepare_bundle(
    root: &Path,
    profile_path: &Path,
    placement_name: &str,
    repo_root: &Path,
    output_dir: &Path,
) -> Result<()> {
    let (profile, canonical) = resolve(root, profile_path, &mut Vec::new())?;
    validate(&profile, Some(placement_name))?;
    ensure!(
        profile.status == Some(Status::Runtime),
        "only status=runtime profiles can enter a build bundle"
    );
    ensure!(
        profile.target.as_ref().unwrap().boot_protocol.as_deref() == Some("fdt-direct"),
        "build bundles currently require the fdt-direct boot protocol"
    );
    let host_build = profile
        .host
        .as_ref()
        .and_then(|host| host.build.as_ref())
        .context("runtime profile requires host.build metadata")?;
    let placement = &profile.placements[placement_name];
    let kernel = artifact_path(&profile, "kernel", repo_root)?;
    let initrd = profile
        .artifacts
        .get("initrd")
        .map(|_| artifact_path(&profile, "initrd", repo_root))
        .transpose()?;
    ensure!(
        kernel.is_file(),
        "guest kernel is not staged: {}",
        kernel.display()
    );
    if let Some(path) = &initrd {
        ensure!(
            path.is_file(),
            "guest initrd is not staged: {}",
            path.display()
        );
    }

    let checked_initrd_total = match host_build.initrd_total_bytes {
        Some(expected) => {
            let media_cache = host_build
                .media_initrd_cache
                .as_deref()
                .context("host.build.initrd_total_bytes requires media_initrd_cache")?;
            let media_path = confined_repo_path(repo_root, media_cache)?;
            let media_bytes = fs::metadata(&media_path)
                .with_context(|| format!("reading media initrd {}", media_path.display()))?
                .len();
            let overlay_bytes = initrd
                .as_ref()
                .map(|path| fs::metadata(path).map(|metadata| metadata.len()))
                .transpose()?
                .unwrap_or(0);
            let actual = media_bytes
                .checked_add(overlay_bytes)
                .context("media initrd plus overlay size overflow")?;
            ensure!(
                actual == expected,
                "host.build.initrd_total_bytes mismatch: declared {expected}, media {media_bytes} + overlay {overlay_bytes} = {actual}"
            );
            Some(actual)
        }
        None => None,
    };

    fs::create_dir_all(output_dir)
        .with_context(|| format!("creating build bundle {}", output_dir.display()))?;
    let dtb = render_profile_dtb(
        &profile,
        placement,
        host_build,
        initrd.as_deref(),
        checked_initrd_total,
        repo_root,
        output_dir,
    )?;
    let dtb_cache = artifact_path(&profile, "dtb", repo_root)?;
    if let Some(parent) = dtb_cache.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::copy(&dtb, &dtb_cache).with_context(|| {
        format!(
            "copying rendered DTB {} to {}",
            dtb.display(),
            dtb_cache.display()
        )
    })?;
    verify_artifacts(&profile, placement_name, repo_root)?;

    fs::copy(&kernel, output_dir.join("kernel.bin"))?;
    fs::copy(&dtb, output_dir.join("guest.dtb"))?;
    let packaged_initrd = output_dir.join("initrd.bin");
    if initrd.is_none() {
        fs::write(&packaged_initrd, [])?;
    } else {
        fs::copy(initrd.as_ref().unwrap(), &packaged_initrd)?;
    }
    fs::write(
        output_dir.join("profile.bin"),
        compile(&profile, &canonical, placement_name)?,
    )?;
    fs::write(
        output_dir.join("profile_build.h"),
        format!(
            "#ifndef AGENTOS_GUEST_PROFILE_BUILD_H\n#define AGENTOS_GUEST_PROFILE_BUILD_H\n#include <stdint.h>\n#define AGENTOS_GUEST_INITRD_TOTAL_BYTES UINT64_C({})\n#endif\n",
            checked_initrd_total.unwrap_or(0)
        ),
    )?;
    println!(
        "[guest-profile] prepared {} in {}",
        profile.id.as_deref().unwrap_or("unknown"),
        output_dir.display()
    );
    Ok(())
}

fn artifact_path(profile: &Profile, name: &str, repo_root: &Path) -> Result<PathBuf> {
    let cache = required(
        &profile.artifacts[name].cache,
        &format!("artifacts.{name}.cache"),
    )?;
    Ok(repo_root.join(cache))
}

fn render_profile_dtb(
    profile: &Profile,
    placement: &Placement,
    build: &HostBuild,
    initrd: Option<&Path>,
    checked_initrd_total: Option<u64>,
    repo_root: &Path,
    output_dir: &Path,
) -> Result<PathBuf> {
    let command_line = build
        .bootargs
        .as_deref()
        .or_else(|| {
            profile
                .boot
                .as_ref()
                .and_then(|boot| boot.command_line.as_deref())
        })
        .context("boot.command_line is required")?;
    ensure!(
        !command_line
            .chars()
            .any(|ch| matches!(ch, '\0' | '\n' | '\r' | '"' | '\\')),
        "boot.command_line contains characters unsafe for a DTS string"
    );
    let gpa = placement.guest_gpa_base.unwrap();
    let ram = placement.ram_size.unwrap();
    let initrd_start = placement.initrd_load_address.unwrap_or(0);
    let packaged_initrd_size = initrd
        .map(|path| fs::metadata(path).map(|metadata| metadata.len()))
        .transpose()?
        .unwrap_or(0);
    let initrd_size = checked_initrd_total.unwrap_or(packaged_initrd_size);
    let initrd_end = initrd_start
        .checked_add(initrd_size)
        .context("initrd end address overflow")?;
    let substitutions = [
        ("@GUEST_RAM_NODE@", format!("{gpa:x}")),
        ("@GUEST_RAM_BASE@", format!("0x{gpa:x}")),
        ("@GUEST_RAM_SIZE@", format!("0x{ram:x}")),
        ("@GUEST_INITRD_START@", format!("0x{initrd_start:x}")),
        ("@GUEST_INITRD_END@", format!("0x{initrd_end:x}")),
        ("@GUEST_BOOTARGS@", command_line.to_string()),
        (
            "@GUEST_GPU_NODE@",
            if profile
                .target
                .as_ref()
                .and_then(|t| t.devices.as_ref())
                .is_some_and(|devices| devices.iter().any(|d| d == "gpu"))
            {
                include_str!("../../kernel/agentos-root-task/virtio-gpu-guest.dts.inc").to_string()
            } else {
                String::new()
            },
        ),
        (
            "@GUEST_INPUT_NODE@",
            if profile
                .target
                .as_ref()
                .and_then(|t| t.devices.as_ref())
                .is_some_and(|devices| devices.iter().any(|d| d == "input"))
            {
                include_str!("../../kernel/agentos-root-task/virtio-input-guest.dts.inc")
                    .to_string()
            } else {
                String::new()
            },
        ),
    ];
    let template_path = confined_repo_path(
        repo_root,
        build
            .template
            .as_deref()
            .context("FDT adapter requires template")?,
    )?;
    let template = render_dts_template(&fs::read_to_string(&template_path)?, &substitutions)?;
    let overlay_path = output_dir.join("guest-overlay.dts");
    fs::write(&overlay_path, template)?;

    let dts_path = output_dir.join("guest.dts");
    match build.adapter.as_str() {
        "linux-merge" => {
            let base_rel = build.base.as_deref().context("linux-merge requires base")?;
            let base_path = confined_repo_path(repo_root, base_rel)?;
            let original = fs::read_to_string(&base_path)?;
            let old_memory = "memory@40000000";
            let old_reg = "0x00 0x40000000 0x00 0x80000000";
            ensure!(
                original.contains(old_memory) && original.contains(old_reg),
                "Linux base DTS does not contain its bounded memory placeholders"
            );
            let base = original
                .replace(old_memory, &format!("memory@{gpa:x}"))
                .replace(old_reg, &format!("0x00 0x{gpa:x} 0x00 0x{ram:x}"));
            let base_rendered = output_dir.join("guest-base.dts");
            fs::write(&base_rendered, base)?;
            let output = Command::new(repo_root.join("libvmm/tools/dtscat"))
                .arg(&base_rendered)
                .arg(&overlay_path)
                .output()
                .context("running bounded Linux DTS merge")?;
            ensure!(
                output.status.success(),
                "Linux DTS merge failed: {}",
                String::from_utf8_lossy(&output.stderr)
            );
            fs::write(&dts_path, output.stdout)?;
        }
        "fdt-template" => fs::copy(&overlay_path, &dts_path).map(|_| ())?,
        other => anyhow::bail!("unsupported host.build adapter {other:?}"),
    }

    let dtb_path = output_dir.join("rendered.dtb");
    let output = Command::new("dtc")
        .args(["-q", "-I", "dts", "-O", "dtb"])
        .arg(&dts_path)
        .output()
        .context("running dtc for guest profile")?;
    ensure!(
        output.status.success(),
        "guest DTB compilation failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    fs::write(&dtb_path, output.stdout)?;
    Ok(dtb_path)
}

fn render_dts_template(source: &str, substitutions: &[(&str, String)]) -> Result<String> {
    let mut rendered = source.to_string();
    for (token, value) in substitutions {
        rendered = rendered.replace(token, value);
    }
    ensure!(
        !rendered.contains("@GUEST_"),
        "guest DTS template contains an unresolved variable"
    );
    Ok(rendered)
}

fn confined_repo_path(repo_root: &Path, value: &str) -> Result<PathBuf> {
    let path = Path::new(value);
    ensure!(
        !path.is_absolute()
            && path
                .components()
                .all(|part| matches!(part, Component::Normal(_))),
        "host.build paths must remain beneath --repo-root"
    );
    Ok(repo_root.join(path))
}

fn verify_artifacts(profile: &Profile, placement_name: &str, repo_root: &Path) -> Result<()> {
    let placement = &profile.placements[placement_name];
    for (name, artifact) in &profile.artifacts {
        let cache = required(&artifact.cache, &format!("artifacts.{name}.cache"))?;
        let expected = artifact_hash(artifact, placement, name)?;
        let path = repo_root.join(cache);
        let mut file = fs::File::open(&path)
            .with_context(|| format!("opening {name} artifact {}", path.display()))?;
        ensure!(
            file.metadata()?.len() <= artifact.max_bytes.unwrap(),
            "{name} artifact exceeds max_bytes: {}",
            path.display()
        );
        let mut hash = Sha256::new();
        let mut buffer = [0u8; 64 * 1024];
        loop {
            let count = file
                .read(&mut buffer)
                .with_context(|| format!("hashing {}", path.display()))?;
            if count == 0 {
                break;
            }
            hash.update(&buffer[..count]);
        }
        ensure!(
            hash.finalize().as_slice() == expected,
            "{name} artifact SHA-256 mismatch: {}",
            path.display()
        );
    }
    Ok(())
}

fn profile_files(root: &Path) -> Result<Vec<PathBuf>> {
    fn visit(root: &Path, dir: &Path, out: &mut Vec<PathBuf>) -> Result<()> {
        for entry in fs::read_dir(dir).with_context(|| format!("reading {}", dir.display()))? {
            let path = entry?.path();
            if path.is_dir() {
                visit(root, &path, out)?;
            } else if path.extension().and_then(|s| s.to_str()) == Some("toml") {
                out.push(path.strip_prefix(root)?.to_path_buf());
            }
        }
        Ok(())
    }
    let mut files = Vec::new();
    visit(root, root, &mut files)?;
    files.sort();
    Ok(files)
}

fn safe_relative(path: &Path) -> Result<()> {
    ensure!(
        !path.is_absolute(),
        "profile paths must be relative to the profile root"
    );
    ensure!(
        path.components().all(|c| matches!(c, Component::Normal(_))),
        "profile path contains a forbidden component: {}",
        path.display()
    );
    ensure!(
        path.extension().and_then(|s| s.to_str()) == Some("toml"),
        "profile path must end in .toml"
    );
    Ok(())
}

fn resolve(root: &Path, relative: &Path, stack: &mut Vec<PathBuf>) -> Result<(Profile, String)> {
    safe_relative(relative)?;
    ensure!(
        stack.len() < MAX_INHERITANCE_DEPTH,
        "profile inheritance exceeds {MAX_INHERITANCE_DEPTH} levels"
    );
    ensure!(
        !stack.iter().any(|p| p == relative),
        "profile inheritance cycle at {}",
        relative.display()
    );
    stack.push(relative.to_path_buf());
    let path = root.join(relative);
    let text = fs::read_to_string(&path).with_context(|| format!("reading {}", path.display()))?;
    let mut child: toml::Value =
        toml::from_str(&text).with_context(|| format!("parsing {}", path.display()))?;
    let parent_name = child
        .get("extends")
        .and_then(toml::Value::as_str)
        .map(str::to_owned);
    let merged = if let Some(parent_name) = parent_name {
        let parent_path = PathBuf::from(parent_name);
        let (_, parent_canonical) = resolve(root, &parent_path, stack)?;
        let mut parent: toml::Value = toml::from_str(&parent_canonical)?;
        merge(&mut parent, child);
        parent
    } else {
        child.as_table_mut().map(|t| t.remove("extends"));
        child
    };
    stack.pop();
    let mut merged = merged;
    merged.as_table_mut().map(|t| t.remove("extends"));
    let canonical = toml::to_string(&merged)?;
    let profile: Profile = merged.try_into().context("decoding resolved profile")?;
    Ok((profile, canonical))
}

fn merge(base: &mut toml::Value, overlay: toml::Value) {
    match (base, overlay) {
        (toml::Value::Table(base), toml::Value::Table(overlay)) => {
            for (key, value) in overlay {
                match base.get_mut(&key) {
                    Some(existing) => merge(existing, value),
                    None => {
                        base.insert(key, value);
                    }
                }
            }
        }
        (base, overlay) => *base = overlay,
    }
}

fn valid_alias(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 63
        && value
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-')
}

fn validate(profile: &Profile, placement: Option<&str>) -> Result<()> {
    ensure!(profile.schema == Some(VERSION), "schema must be {VERSION}");
    let id = required(&profile.id, "id")?;
    ensure!(
        !id.is_empty() && id.len() <= 63 && id.is_ascii(),
        "id must be 1..63 ASCII bytes"
    );
    let mut aliases = BTreeSet::new();
    for alias in &profile.aliases {
        ensure!(valid_alias(alias), "invalid guest profile alias {alias:?}");
        ensure!(
            aliases.insert(alias),
            "duplicate guest profile alias {alias:?}"
        );
    }
    let status = profile.status.context("status is required")?;
    validate_host(profile.host.as_ref())?;
    if status == Status::Abstract {
        return Ok(());
    }

    let target = profile
        .target
        .as_ref()
        .context("target table is required")?;
    enum_value(
        required(&target.architecture, "target.architecture")?,
        &["aarch64", "x86-64", "riscv64"],
    )?;
    enum_value(
        required(&target.boot_protocol, "target.boot_protocol")?,
        &["fdt-direct", "uefi", "process"],
    )?;
    if profile
        .host
        .as_ref()
        .and_then(|h| h.build.as_ref())
        .is_some_and(|b| b.adapter == "uefi-artifacts")
    {
        ensure!(
            target.boot_protocol.as_deref() == Some("uefi"),
            "uefi-artifacts requires the uefi boot protocol"
        );
    }
    enum_value(
        required(&target.kernel_format, "target.kernel_format")?,
        &["linux-image", "raw", "elf", "uefi"],
    )?;
    let kernel_format = target.kernel_format.as_ref().unwrap();
    let entry_from_image = target
        .entry_from_image
        .context("target.entry_from_image is required")?;
    ensure!(
        (kernel_format == "linux-image") == entry_from_image,
        "linux-image requires entry_from_image=true and other formats require false"
    );
    target.autostart.context("target.autostart is required")?;
    ensure!(
        target.vcpus.is_some_and(|n| (1..=MAX_VCPUS).contains(&n)),
        "target.vcpus must be 1..8"
    );
    validate_cpu_features(target.cpu_features.as_ref())?;
    ensure!(
        target
            .control_type
            .is_some_and(|value| (1..=u8::MAX as u32).contains(&value)),
        "target.control_type must fit the nonzero control-plane wire field"
    );
    target.guest_id.context("target.guest_id is required")?;
    let devices = target
        .devices
        .as_ref()
        .context("target.devices is required")?;
    ensure!(
        devices.iter().any(|d| d == "console"),
        "target.devices must contain console"
    );
    let mut unique = BTreeSet::new();
    for device in devices {
        enum_value(
            device,
            &["net", "block", "console", "gpu", "input", "sound"],
        )?;
        ensure!(unique.insert(device), "duplicate target device {device}");
    }
    ensure!(
        devices.iter().any(|d| d == "net") == target.network_client.is_some(),
        "network_client must be set exactly when net is present"
    );
    ensure!(
        devices.iter().any(|d| d == "block") == target.block_media.is_some(),
        "block_media must be set exactly when block is present"
    );
    let cmdline = profile
        .boot
        .as_ref()
        .and_then(|b| b.command_line.as_ref())
        .context("boot.command_line is required")?;
    ensure!(
        cmdline.len() <= 255 && cmdline.is_ascii() && !cmdline.contains('\0'),
        "boot.command_line must be at most 255 ASCII bytes without NUL"
    );
    let media_initrd_path = profile.boot.as_ref().unwrap().media_initrd_path.as_deref();
    if let Some(path) = media_initrd_path {
        ensure!(
            !path.is_empty() && path.len() <= 63 && path.is_ascii(),
            "boot.media_initrd_path must be 1..63 ASCII bytes"
        );
        ensure!(
            !path.starts_with('/')
                && !path.ends_with('/')
                && path
                    .split('/')
                    .all(|part| !part.is_empty() && part != "." && part != ".."),
            "boot.media_initrd_path must be a normalized relative path"
        );
        ensure!(
            profile.artifacts.contains_key("initrd") && devices.iter().any(|d| d == "block"),
            "boot.media_initrd_path requires initrd and block device"
        );
    }

    validate_artifact(profile, "kernel", status == Status::Runtime)?;
    if profile.artifacts.contains_key("dtb") || target.boot_protocol.as_deref() != Some("uefi") {
        validate_artifact(profile, "dtb", status == Status::Runtime)?;
    }
    if profile.artifacts.contains_key("initrd") {
        validate_artifact(profile, "initrd", status == Status::Runtime)?;
    }
    if let Some(total) = profile
        .host
        .as_ref()
        .and_then(|host| host.build.as_ref())
        .and_then(|build| build.initrd_total_bytes)
    {
        let initrd_bound = profile
            .artifacts
            .get("initrd")
            .and_then(|artifact| artifact.max_bytes)
            .context("host.build.initrd_total_bytes requires a bounded initrd artifact")?;
        ensure!(
            total <= initrd_bound,
            "host.build.initrd_total_bytes must fit artifacts.initrd.max_bytes"
        );
    }
    ensure!(
        profile
            .artifacts
            .keys()
            .all(|k| matches!(k.as_str(), "kernel" | "dtb" | "initrd")),
        "unknown target artifact"
    );
    ensure!(
        !profile.placements.is_empty(),
        "at least one placement is required"
    );
    for (name, value) in &profile.placements {
        validate_placement(name, value, profile.artifacts.contains_key("dtb"))?;
        validate_artifact_windows(profile, name, value)?;
    }
    if let Some(name) = placement {
        ensure!(
            profile.placements.contains_key(name),
            "placement {name:?} is not defined"
        );
        validate_selected_placements([(
            name,
            profile.placements.get(name).expect("placement was checked"),
        )])?;
    }
    Ok(())
}

fn validate_cpu_features(features: Option<&CpuFeatures>) -> Result<()> {
    let Some(features) = features else {
        return Ok(());
    };
    ensure!(
        features.version == Some(CPU_FEATURES_VERSION),
        "target.cpu_features.version must be {CPU_FEATURES_VERSION}"
    );
    let mut required = BTreeSet::new();
    let mut prohibited = BTreeSet::new();
    for feature in &features.required {
        enum_value(
            feature,
            &["fp", "simd", "crypto", "rng", "vector", "nested-virt"],
        )?;
        ensure!(
            required.insert(feature),
            "duplicate required CPU feature {feature}"
        );
    }
    for feature in &features.prohibited {
        enum_value(
            feature,
            &["fp", "simd", "crypto", "rng", "vector", "nested-virt"],
        )?;
        ensure!(
            prohibited.insert(feature),
            "duplicate prohibited CPU feature {feature}"
        );
        ensure!(
            !required.contains(feature),
            "CPU feature {feature} cannot be both required and prohibited"
        );
    }
    Ok(())
}

fn validate_selected_placements<'a>(
    placements: impl IntoIterator<Item = (&'a str, &'a Placement)>,
) -> Result<()> {
    let placements: Vec<_> = placements.into_iter().collect();
    ensure!(
        !placements.is_empty() && placements.len() <= MAX_SELECTED_PLACEMENTS,
        "selected placement count must be 1..={MAX_SELECTED_PLACEMENTS}"
    );
    let mut total_ram = 0u64;
    for (index, (name, placement)) in placements.iter().enumerate() {
        let gpa = placement.guest_gpa_base.expect("placement validated");
        let hva = placement.vmm_hva_base.expect("placement validated");
        let ram = placement.ram_size.expect("placement validated");
        total_ram = total_ram
            .checked_add(ram)
            .filter(|total| *total <= RAM_MAX)
            .with_context(|| "selected placement RAM exceeds resource bound")?;
        for (other_name, other) in &placements[..index] {
            let other_gpa = other.guest_gpa_base.expect("placement validated");
            let other_hva = other.vmm_hva_base.expect("placement validated");
            let other_ram = other.ram_size.expect("placement validated");
            ensure!(
                !ranges_overlap(gpa, ram, other_gpa, other_ram),
                "selected placements {name:?} and {other_name:?} GPA ranges overlap"
            );
            ensure!(
                !ranges_overlap(hva, ram, other_hva, other_ram),
                "selected placements {name:?} and {other_name:?} HVA ranges overlap"
            );
        }
    }
    Ok(())
}

fn validate_artifact_windows(profile: &Profile, name: &str, p: &Placement) -> Result<()> {
    let kernel_address = p.kernel_load_address.unwrap();
    let dtb_address = p.dtb_load_address.unwrap_or(0);
    let kernel_size = profile.artifacts["kernel"].max_bytes.unwrap();
    let dtb_size = profile
        .artifacts
        .get("dtb")
        .and_then(|a| a.max_bytes)
        .unwrap_or(0);
    ensure!(
        !ranges_overlap(kernel_address, kernel_size, dtb_address, dtb_size),
        "placement {name:?} kernel and DTB maximum windows overlap"
    );
    if let Some(initrd) = profile.artifacts.get("initrd") {
        let initrd_address = p
            .initrd_load_address
            .with_context(|| format!("placement {name:?} requires initrd_load_address"))?;
        let initrd_size = initrd.max_bytes.unwrap();
        ensure!(
            !ranges_overlap(kernel_address, kernel_size, initrd_address, initrd_size)
                && !ranges_overlap(dtb_address, dtb_size, initrd_address, initrd_size),
            "placement {name:?} artifact maximum windows overlap"
        );
    }
    Ok(())
}

fn ranges_overlap(a: u64, a_size: u64, b: u64, b_size: u64) -> bool {
    if a_size == 0 || b_size == 0 {
        return false;
    }
    match (a.checked_add(a_size), b.checked_add(b_size)) {
        (Some(a_end), Some(b_end)) => a < b_end && b < a_end,
        _ => true,
    }
}

fn validate_artifact(profile: &Profile, name: &str, pinned: bool) -> Result<()> {
    let artifact = profile
        .artifacts
        .get(name)
        .with_context(|| format!("artifacts.{name} is required"))?;
    ensure!(
        artifact.max_bytes.is_some_and(|n| n > 0),
        "artifacts.{name}.max_bytes must be positive"
    );
    if pinned {
        let source = required(&artifact.source, &format!("artifacts.{name}.source"))?;
        let cache = required(&artifact.cache, &format!("artifacts.{name}.cache"))?;
        ensure!(
            !source.is_empty() && source.len() <= 2048,
            "artifacts.{name}.source exceeds bounds"
        );
        let cache_path = Path::new(cache);
        ensure!(
            !cache_path.is_absolute()
                && cache_path
                    .components()
                    .all(|part| matches!(part, Component::Normal(_))),
            "artifacts.{name}.cache must remain beneath --repo-root"
        );
        parse_hash(required(
            &artifact.sha256,
            &format!("artifacts.{name}.sha256"),
        )?)?;
    } else if let Some(hash) = &artifact.sha256 {
        parse_hash(hash)?;
    }
    Ok(())
}

fn validate_placement(name: &str, p: &Placement, has_dtb: bool) -> Result<()> {
    ensure!(
        !name.is_empty() && name.len() <= 63,
        "invalid placement name"
    );
    let gpa = p
        .guest_gpa_base
        .context("placement.guest_gpa_base is required")?;
    let hva = p
        .vmm_hva_base
        .context("placement.vmm_hva_base is required")?;
    let ram = p.ram_size.context("placement.ram_size is required")?;
    ensure!(
        gpa & 0xfff == 0 && hva & 0xfff == 0,
        "placement bases must be page aligned"
    );
    ensure!(
        (RAM_MIN..=RAM_MAX).contains(&ram) && ram & (RAM_ALIGN - 1) == 0,
        "placement RAM must be 2 MiB..=8 GiB and 2 MiB aligned"
    );
    ensure!(
        gpa.checked_add(ram).is_some() && hva.checked_add(ram).is_some(),
        "placement address range wraps"
    );
    for (field, address) in [
        ("kernel_load_address", p.kernel_load_address),
        ("dtb_load_address", p.dtb_load_address),
    ] {
        if field == "dtb_load_address" && !has_dtb {
            ensure!(
                address.unwrap_or(0) == 0 && p.dtb_sha256.is_none(),
                "placement without a DTB must not specify its address or hash"
            );
            continue;
        }
        let address = address.with_context(|| format!("placement.{field} is required"))?;
        ensure!(
            address >= gpa && address < gpa + ram,
            "placement.{field} lies outside RAM"
        );
    }
    for hash in [&p.kernel_sha256, &p.dtb_sha256, &p.initrd_sha256]
        .into_iter()
        .flatten()
    {
        parse_hash(hash)?;
    }
    Ok(())
}

fn validate_host(host: Option<&Host>) -> Result<()> {
    let Some(host) = host else { return Ok(()) };
    if let Some(seed) = &host.seed {
        enum_value(&seed.adapter, &["nocloud-debian-v1"])?;
        for path in [&seed.root_ext4, &seed.disk_raw] {
            ensure!(
                !path.is_empty() && path.len() <= 255,
                "invalid seed source path"
            );
            confined_repo_path(Path::new("."), path)?;
        }
        ensure!(
            seed.root_ext4 != seed.disk_raw,
            "seed sources must be distinct"
        );
        ensure!(
            seed.partition_offset > 0 && seed.partition_offset % 512 == 0,
            "seed partition offset must be positive and sector aligned"
        );
        let qemu = host.qemu.as_ref().context("seed requires host.qemu")?;
        ensure!(
            qemu.media.iter().filter(|media| media.writable).count() == 1,
            "seed requires exactly one writable disk"
        );
        let ssh = qemu.ssh.as_ref().context("seed requires host.qemu.ssh")?;
        ensure!(
            ssh.account == "debian",
            "NoCloud adapter requires the Debian account"
        );
        crate::cmd_seed_guest::validate_guest_address(
            ssh.guest_address
                .as_deref()
                .context("NoCloud seed requires a guest address")?
                .parse()?,
        )?;
        ensure!(
            host.provision.is_empty(),
            "NoCloud seed cannot also use console provisioning"
        );
    }
    if let Some(qemu) = &host.qemu {
        validate_qemu(qemu)?;
    }
    if let Some(console) = &host.console {
        enum_value(&console.adapter, &["expect"])?;
        for (field, markers) in [
            ("success", &console.success),
            ("require", &console.require),
            ("reject", &console.reject),
        ] {
            ensure!(
                markers.len() <= 16,
                "host.console.{field} exceeds 16 markers"
            );
            for marker in markers {
                ensure!(
                    !marker.is_empty() && marker.len() <= 255,
                    "host.console.{field} contains an invalid marker"
                );
            }
        }
        ensure!(
            console.interaction.len() <= 32,
            "host.console.interaction exceeds 32 rules"
        );
        for interaction in &console.interaction {
            ensure!(
                interaction.when.len() <= 8
                    && interaction
                        .when
                        .iter()
                        .all(|marker| !marker.is_empty() && marker.len() <= 255),
                "host.console.interaction.when is invalid"
            );
            ensure!(
                !interaction.when.is_empty() || interaction.after_secs.is_some(),
                "host.console.interaction needs when markers or after_secs"
            );
            ensure!(
                !interaction.send.is_empty() && interaction.send.len() <= 1024,
                "host.console.interaction.send is invalid"
            );
            ensure!(
                (1..=8).contains(&interaction.max_fires),
                "host.console.interaction.max_fires must be 1..8"
            );
            ensure!(
                interaction.after_secs.is_none_or(|seconds| seconds <= 3600),
                "host.console.interaction.after_secs exceeds 3600"
            );
        }
        ensure!(
            console.probe_line.is_some() == console.probe_marker.is_some(),
            "host.console.probe_line and probe_marker must be set together"
        );
        if let Some(probe) = &console.probe_line {
            ensure!(
                !probe.is_empty() && probe.len() <= 1024,
                "host.console.probe_line is invalid"
            );
        }
        if let Some(marker) = &console.probe_marker {
            ensure!(
                !marker.is_empty() && marker.len() <= 255,
                "host.console.probe_marker is invalid"
            );
        }
        ensure!(
            console
                .probe_timeout_secs
                .is_none_or(|seconds| (1..=3600).contains(&seconds)),
            "host.console.probe_timeout_secs must be 1..3600"
        );
    }
    if let Some(desktop) = &host.desktop {
        enum_value(&desktop.adapter, &["rfb-over-ssh"])?;
        let has_guest_port = desktop.guest_port.is_some();
        let has_guest_socket = desktop.guest_socket.is_some();
        ensure!(
            has_guest_port != has_guest_socket,
            "host.desktop requires exactly one of guest_port or guest_socket"
        );
        ensure!(
            desktop.local_port != 0,
            "host.desktop.local_port must be nonzero"
        );
        if let Some(guest_port) = desktop.guest_port {
            ensure!(
                guest_port != 0 && desktop.local_port != guest_port,
                "host.desktop ports must be nonzero and distinct"
            );
        }
        if let Some(guest_socket) = &desktop.guest_socket {
            ensure!(
                guest_socket.starts_with('/')
                    && guest_socket.len() <= 100
                    && !guest_socket
                        .chars()
                        .any(|ch| matches!(ch, ':' | '\n' | '\r' | '\0'))
                    && Path::new(guest_socket)
                        .components()
                        .all(|component| matches!(
                            component,
                            std::path::Component::RootDir | std::path::Component::Normal(_)
                        )),
                "host.desktop.guest_socket must be a bounded absolute path"
            );
        }
        for (field, seconds) in [
            ("provision_timeout_secs", desktop.provision_timeout_secs),
            ("frame_timeout_secs", desktop.frame_timeout_secs),
            ("io_timeout_secs", desktop.io_timeout_secs),
        ] {
            ensure!(
                (1..=3600).contains(&seconds),
                "host.desktop.{field} must be 1..3600"
            );
        }
        ensure!(
            !desktop.provision_script.is_empty()
                && desktop.provision_script.len() <= 16 * 1024
                && !desktop.provision_script.contains('\0'),
            "host.desktop.provision_script is invalid or exceeds 16 KiB"
        );
        ensure!(
            host.qemu
                .as_ref()
                .and_then(|qemu| qemu.ssh.as_ref())
                .is_some(),
            "host.desktop requires host.qemu.ssh"
        );
    }
    if let Some(build) = &host.build {
        enum_value(
            &build.adapter,
            &["linux-merge", "fdt-template", "uefi-artifacts"],
        )?;
        if build.adapter == "uefi-artifacts" {
            ensure!(
                build.template.is_none()
                    && build.base.is_none()
                    && build.bootargs.is_none()
                    && build.initrd_total_bytes.is_none()
                    && build.media_initrd_cache.is_none(),
                "uefi-artifacts does not accept FDT template fields"
            );
        } else {
            validate_repo_relative(
                build
                    .template
                    .as_deref()
                    .context("FDT adapter requires template")?,
                "host.build.template",
            )?;
        }
        validate_repo_relative(&build.acquire_dir, "host.build.acquire_dir")?;
        match (build.adapter.as_str(), build.base.as_deref()) {
            ("linux-merge", Some(base)) => {
                validate_repo_relative(base, "host.build.base")?;
            }
            ("linux-merge", None) => anyhow::bail!("linux-merge requires host.build.base"),
            ("fdt-template", None) => {}
            ("uefi-artifacts", None) => {}
            ("fdt-template", Some(_)) => {
                anyhow::bail!("fdt-template does not accept host.build.base")
            }
            _ => unreachable!(),
        }
        if let Some(bootargs) = &build.bootargs {
            ensure!(
                !bootargs.is_empty()
                    && bootargs.len() <= 1024
                    && bootargs.is_ascii()
                    && !bootargs
                        .chars()
                        .any(|ch| matches!(ch, '\0' | '\n' | '\r' | '"' | '\\')),
                "host.build.bootargs is not a bounded DTS string"
            );
        }
        if let Some(total) = build.initrd_total_bytes {
            ensure!(total > 0, "host.build.initrd_total_bytes must be nonzero");
            ensure!(
                build.media_initrd_cache.is_some(),
                "host.build.initrd_total_bytes requires media_initrd_cache"
            );
        } else {
            ensure!(
                build.media_initrd_cache.is_none(),
                "host.build.media_initrd_cache requires initrd_total_bytes"
            );
        }
        if let Some(path) = &build.media_initrd_cache {
            validate_repo_relative(path, "host.build.media_initrd_cache")?;
        }
    }
    for (recipe_name, recipe) in [
        ("acquire", &host.acquire),
        ("provision", &host.provision),
        ("test", &host.test),
    ] {
        ensure!(
            recipe.len() <= MAX_RECIPE_STEPS,
            "host.{recipe_name} exceeds {MAX_RECIPE_STEPS} steps"
        );
        for step in recipe {
            ensure!(
                HOST_ACTIONS.contains(&step.action.as_str()),
                "unsupported host action {:?}",
                step.action
            );
            ensure!(step.args.len() <= 16, "host action has too many arguments");
            for (key, value) in &step.args {
                ensure!(
                    !key.is_empty() && key.len() <= 63 && value.len() <= 1024,
                    "host action argument exceeds bounds"
                );
            }
            validate_host_action(step)?;
        }
    }
    Ok(())
}

fn validate_repo_relative(value: &str, field: &str) -> Result<()> {
    let path = Path::new(value);
    ensure!(
        !value.is_empty()
            && value.len() <= 255
            && !path.is_absolute()
            && path
                .components()
                .all(|part| matches!(part, Component::Normal(_))),
        "{field} must be a confined repository-relative path"
    );
    Ok(())
}

fn validate_qemu(qemu: &Qemu) -> Result<()> {
    enum_value(
        &qemu.board,
        &[
            "qemu_virt_aarch64",
            "qemu_virt_riscv64",
            "x86_64_generic",
            "x86_64_generic_vtx",
        ],
    )?;
    ensure!(
        !qemu.machine.is_empty()
            && qemu.machine.len() <= 127
            && qemu
                .machine
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || b",=._-".contains(&byte)),
        "host.qemu.machine contains unsupported characters"
    );
    ensure!(
        qemu.memory.len() >= 2
            && qemu.memory.len() <= 8
            && matches!(qemu.memory.as_bytes().last(), Some(b'M' | b'G'))
            && qemu.memory[..qemu.memory.len() - 1]
                .parse::<u32>()
                .is_ok_and(|value| value > 0),
        "host.qemu.memory must be a positive MiB/GiB quantity"
    );
    ensure!(qemu.media.len() <= 8, "host.qemu.media exceeds 8 entries");
    let mut buses = BTreeSet::new();
    let mut drive_ids = BTreeSet::new();
    for media in &qemu.media {
        let path = Path::new(&media.path);
        ensure!(
            !path.is_absolute()
                && path
                    .components()
                    .all(|part| matches!(part, Component::Normal(_))),
            "host.qemu.media path must remain beneath the repository root"
        );
        ensure!(
            valid_identifier(&media.drive_id),
            "host.qemu.media drive_id is invalid"
        );
        ensure!(buses.insert(media.bus), "duplicate host.qemu.media bus");
        ensure!(
            drive_ids.insert(&media.drive_id),
            "duplicate host.qemu.media drive_id"
        );
        ensure!(
            media.override_env.len() <= 8
                && media.override_env.iter().all(|value| valid_env_name(value)),
            "host.qemu.media override_env contains an invalid name"
        );
    }
    if let Some(ssh) = &qemu.ssh {
        ensure!(
            valid_identifier(&ssh.account),
            "host.qemu.ssh account is invalid"
        );
        ensure!(
            ssh.host_port != 0,
            "host.qemu.ssh host_port must be nonzero"
        );
        if let Some(address) = &ssh.guest_address {
            ensure!(
                address.parse::<std::net::Ipv4Addr>().is_ok(),
                "host.qemu.ssh guest_address must be IPv4"
            );
        }
    }
    Ok(())
}

fn valid_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 63
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'-')
}

fn valid_env_name(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 63
        && value.bytes().enumerate().all(|(index, byte)| {
            byte == b'_' || byte.is_ascii_uppercase() || (index > 0 && byte.is_ascii_digit())
        })
}

pub(crate) fn frame_pixel_expectation(step: &RecipeStep) -> Result<(usize, usize, Vec<[u8; 3]>)> {
    let x: usize = step.args.get("x").context("frame x missing")?.parse()?;
    let y: usize = step.args.get("y").context("frame y missing")?.parse()?;
    let hex = step.args.get("rgb").context("frame RGB missing")?;
    ensure!(
        x < 1024 && y < 768,
        "frame coordinate exceeds supported bounds"
    );
    ensure!(
        !hex.is_empty()
            && hex.len() <= 96
            && hex.len() % 6 == 0
            && hex.bytes().all(|b| b.is_ascii_hexdigit()),
        "frame RGB must contain 1..16 RGB hex triples"
    );
    let mut pixels = Vec::new();
    for i in (0..hex.len()).step_by(6) {
        pixels.push([
            u8::from_str_radix(&hex[i..i + 2], 16)?,
            u8::from_str_radix(&hex[i + 2..i + 4], 16)?,
            u8::from_str_radix(&hex[i + 4..i + 6], 16)?,
        ]);
    }
    Ok((x, y, pixels))
}

fn validate_host_action(step: &RecipeStep) -> Result<()> {
    let (required_args, optional_args): (&[&str], &[&str]) = match step.action.as_str() {
        "stage-url" => (
            &["cache_name", "output", "url"],
            &["override_env", "sha256", "sha512"],
        ),
        "download-tar-member" => (&["url", "member", "output"], &[]),
        "build-static-linux-elf" => (&["source", "output", "architecture"], &[]),
        "extract-iso-file" => (&["source", "member", "output"], &["min_bytes"]),
        "extract-arm64-linux-image" | "extract-arm64-elf-image" => {
            (&["source", "member", "output"], &[])
        }
        "build-initramfs-file" => (
            &["output", "path", "mode"],
            &["compression", "content", "content_file", "content_sha256"],
        ),
        "append-initramfs-file" => (
            &["source", "output", "path", "mode"],
            &["compression", "content", "content_file", "content_sha256"],
        ),
        "filter-initramfs-modules" => (&["source", "output", "modules"], &[]),
        "convert-qcow2-raw" => (&["source", "output"], &[]),
        "extract-gpt-partition" => (&["source", "output", "index"], &["sector_size"]),
        "extract-ext4-file" => (&["source", "output", "path"], &[]),
        "install-gpt-ext4-file" => (
            &["source", "output", "index", "path", "content"],
            &["sector_size"],
        ),
        "normalize-arm64-linux-image" => (&["source", "output"], &[]),
        "build-linux-probe-initramfs" => (&["output"], &[]),
        "download" | "verify-sha256" => (&["artifact"], &[]),
        "extract-ufs-file" => (&["member", "artifact"], &[]),
        "decompress-gzip" | "copy" => (&["source", "output"], &[]),
        "build-initramfs" => (&["recipe", "artifact"], &[]),
        "wait-console" | "assert-console" => (&["marker"], &[]),
        "send-console" => (&["text"], &[]),
        "wait-ssh" => (&["account"], &["marker", "prompt"]),
        "ssh-check" => (&["name", "script"], &[]),
        "run-ssh" => (&["recipe"], &[]),
        "assert-virtio" => (&["devices"], &["scope", "console_io"]),
        "assert-frame-pixels" => (&["x", "y", "rgb"], &[]),
        "assert-ssh-output" => (&["command", "stdout"], &[]),
        _ => return Ok(()),
    };
    for key in required_args {
        ensure!(
            step.args.get(*key).is_some_and(|value| !value.is_empty()),
            "host action {:?} requires argument {key:?}",
            step.action
        );
    }
    for key in step.args.keys() {
        ensure!(
            required_args.contains(&key.as_str()) || optional_args.contains(&key.as_str()),
            "host action {:?} has unknown argument {key:?}",
            step.action
        );
    }
    if step.action == "assert-frame-pixels" {
        frame_pixel_expectation(step)?;
    }
    if step.action == "assert-ssh-output" {
        ensure!(
            step.args["command"].len() <= 4096
                && !step.args["command"].contains('\0')
                && step.args["stdout"].len() <= 4096,
            "SSH assertion command and expected output must be bounded text"
        );
    }
    if matches!(step.action.as_str(), "stage-url" | "download-tar-member") {
        ensure!(
            step.args["url"].starts_with("https://"),
            "host downloads must use HTTPS"
        );
    }
    if let Some(value) = step.args.get("sha512") {
        ensure!(
            value.len() == 128
                && value.bytes().all(|byte| byte.is_ascii_hexdigit())
                && value.bytes().any(|byte| byte != b'0'),
            "sha512 must be a nonzero 128-digit hexadecimal digest"
        );
    }
    if let Some(value) = step.args.get("sha256") {
        ensure!(
            value.len() == 64
                && value.bytes().all(|byte| byte.is_ascii_hexdigit())
                && value.bytes().any(|byte| byte != b'0'),
            "sha256 must be a nonzero 64-digit hexadecimal digest"
        );
    }
    if let Some(value) = step.args.get("min_bytes") {
        ensure!(
            value.parse::<u64>().is_ok_and(|number| number > 0),
            "min_bytes must be a positive integer"
        );
    }
    if matches!(
        step.action.as_str(),
        "build-initramfs-file"
            | "append-initramfs-file"
            | "filter-initramfs-modules"
            | "convert-qcow2-raw"
            | "extract-gpt-partition"
            | "extract-ext4-file"
            | "install-gpt-ext4-file"
            | "normalize-arm64-linux-image"
    ) {
        let keys: &[&str] = match step.action.as_str() {
            "build-initramfs-file" => &["output", "path"],
            "append-initramfs-file" => &["source", "output", "path"],
            "filter-initramfs-modules" => &["source", "output"],
            "extract-ext4-file" => &["source", "output"],
            _ => &["source", "output"],
        };
        for key in keys {
            validate_repo_relative(
                &step.args[*key],
                &format!("host action {:?} argument {key:?}", step.action),
            )?;
        }
    }
    if step.action == "build-static-linux-elf" {
        validate_repo_relative(&step.args["source"], "native helper source")?;
        validate_repo_relative(&step.args["output"], "native helper output")?;
        ensure!(
            step.args["source"].ends_with(".c"),
            "native helper source must be C"
        );
        enum_value(&step.args["architecture"], &["x86_64", "aarch64"])?;
    }
    if matches!(
        step.action.as_str(),
        "build-initramfs-file" | "append-initramfs-file"
    ) {
        ensure!(
            step.args.contains_key("content") != step.args.contains_key("content_file"),
            "initramfs requires exactly one of content or content_file"
        );
        if let Some(path) = step.args.get("content_file") {
            validate_repo_relative(path, "initramfs content_file")?;
            let hash = step
                .args
                .get("content_sha256")
                .context("content_file requires content_sha256")?;
            ensure!(
                hash.len() == 64 && hash.bytes().all(|b| b.is_ascii_hexdigit()),
                "content_sha256 must contain 64 hexadecimal digits"
            );
        } else {
            ensure!(
                !step.args.contains_key("content_sha256"),
                "content_sha256 requires content_file"
            );
        }
        ensure!(
            u32::from_str_radix(&step.args["mode"], 8).is_ok_and(|mode| mode <= 0o777),
            "initramfs file mode must be octal and at most 0777"
        );
        if let Some(compression) = step.args.get("compression") {
            enum_value(compression, &["none", "zstd"])?;
        }
    }
    if step.action == "filter-initramfs-modules" {
        let modules: Vec<_> = step.args["modules"].split(',').collect();
        ensure!(
            !modules.is_empty()
                && modules.len() <= 16
                && modules.iter().all(|name| {
                    !name.is_empty()
                        && name.len() <= 64
                        && name
                            .bytes()
                            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
                }),
            "filter-initramfs-modules requires 1..16 bounded module names"
        );
        let mut unique = modules.clone();
        unique.sort_unstable();
        unique.dedup();
        ensure!(unique.len() == modules.len(), "duplicate initramfs module");
    }
    if matches!(
        step.action.as_str(),
        "extract-gpt-partition" | "install-gpt-ext4-file"
    ) {
        ensure!(
            step.args["index"]
                .parse::<u32>()
                .is_ok_and(|index| (1..=128).contains(&index)),
            "extract-gpt-partition index must be 1..=128"
        );
        if let Some(sector_size) = step.args.get("sector_size") {
            enum_value(sector_size, &["512", "4096"])?;
        }
    }
    if matches!(
        step.action.as_str(),
        "extract-ext4-file" | "install-gpt-ext4-file"
    ) {
        let path = Path::new(&step.args["path"]);
        ensure!(
            path.is_absolute()
                && path.components().all(|component| {
                    matches!(component, Component::RootDir | Component::Normal(_))
                })
                && step.args["path"]
                    .bytes()
                    .all(|byte| { byte.is_ascii_alphanumeric() || b"/_+.-".contains(&byte) }),
            "extract-ext4-file path must be confined and absolute"
        );
    }
    if step.action == "install-gpt-ext4-file" {
        ensure!(
            !step.args["content"].is_empty() && step.args["content"].len() <= 65536,
            "installed configuration must contain 1..65536 bytes"
        );
        ensure!(
            step.args["source"] != step.args["output"],
            "disk source and output must differ"
        );
    }
    if step.action == "assert-virtio" {
        for device in step.args["devices"].split(',') {
            enum_value(
                device,
                &["net", "block", "console", "gpu", "input", "sound"],
            )?;
        }
        if let Some(scope) = step.args.get("scope") {
            enum_value(scope, &["emulated", "host-backed"])?;
        }
        if let Some(console_io) = step.args.get("console_io") {
            enum_value(console_io, &["activity", "bidirectional"])?;
        }
    }
    Ok(())
}

fn compile(profile: &Profile, canonical: &str, placement_name: &str) -> Result<Vec<u8>> {
    let target = profile.target.as_ref().unwrap();
    let placement = &profile.placements[placement_name];
    let kernel = &profile.artifacts["kernel"];
    let dtb = profile.artifacts.get("dtb");
    let initrd = profile.artifacts.get("initrd");
    let id = profile.id.as_ref().unwrap();
    let command_line = profile
        .boot
        .as_ref()
        .unwrap()
        .command_line
        .as_ref()
        .unwrap();
    let media_initrd_path = profile
        .boot
        .as_ref()
        .unwrap()
        .media_initrd_path
        .as_deref()
        .unwrap_or("");
    let devices = target.devices.as_ref().unwrap();
    let mut flags = 1u8 << 3;
    if target.autostart.unwrap_or(false) {
        flags |= 1 << 0;
    }
    if initrd.is_some() {
        flags |= 1 << 1;
    }
    if target.entry_from_image.unwrap_or(false) {
        flags |= 1 << 2;
    }
    if !media_initrd_path.is_empty() {
        flags |= 1 << 4;
    }
    let (cpu_feature_version, cpu_required, cpu_prohibited) =
        compile_cpu_features(target.cpu_features.as_ref())?;

    let mut out = Vec::with_capacity(MANIFEST_SIZE);
    push_u64(&mut out, MAGIC);
    push_u16(&mut out, VERSION);
    push_u16(&mut out, MANIFEST_SIZE as u16);
    out.push(map_enum(
        target.architecture.as_ref().unwrap(),
        &["aarch64", "x86-64", "riscv64"],
    )?);
    out.push(map_enum(
        target.boot_protocol.as_ref().unwrap(),
        &["fdt-direct", "uefi", "process"],
    )?);
    out.push(map_enum(
        target.kernel_format.as_ref().unwrap(),
        &["linux-image", "raw", "elf", "uefi"],
    )?);
    out.push(flags);
    push_u32(&mut out, target.guest_id.unwrap());
    push_u32(&mut out, target.vcpus.unwrap());
    let mut device_flags = 0u32;
    for device in devices {
        device_flags |= 1u32
            << (["net", "block", "console", "gpu", "input", "sound"]
                .iter()
                .position(|v| *v == device)
                .unwrap());
    }
    push_u32(&mut out, device_flags);
    push_u16(&mut out, target.network_client.unwrap_or(u16::MAX));
    push_u16(&mut out, target.block_media.unwrap_or(u16::MAX));
    for value in [
        placement.guest_gpa_base.unwrap(),
        placement.vmm_hva_base.unwrap(),
        placement.ram_size.unwrap(),
        placement.kernel_load_address.unwrap(),
        placement.kernel_entry_address.unwrap_or(0),
        placement.dtb_load_address.unwrap_or(0),
        placement.initrd_load_address.unwrap_or(0),
        kernel.max_bytes.unwrap(),
        dtb.and_then(|a| a.max_bytes).unwrap_or(0),
        initrd.and_then(|a| a.max_bytes).unwrap_or(0),
    ] {
        push_u64(&mut out, value);
    }
    out.extend_from_slice(&Sha256::digest(canonical.as_bytes()));
    out.extend_from_slice(&artifact_hash(kernel, placement, "kernel")?);
    if let Some(dtb) = dtb {
        out.extend_from_slice(&artifact_hash(dtb, placement, "dtb")?);
    } else {
        out.extend_from_slice(&[0; 32]);
    }
    if let Some(initrd) = initrd {
        out.extend_from_slice(&artifact_hash(initrd, placement, "initrd")?);
    } else {
        out.extend_from_slice(&[0; 32]);
    }
    push_u16(&mut out, command_line.len() as u16);
    push_u16(&mut out, id.len() as u16);
    push_u32(&mut out, target.control_type.unwrap());
    push_u16(&mut out, media_initrd_path.len() as u16);
    out.push(cpu_feature_version);
    out.push(0);
    push_u16(&mut out, cpu_required);
    push_u16(&mut out, cpu_prohibited);
    push_text(&mut out, id, 64);
    push_text(&mut out, command_line, 256);
    push_text(&mut out, media_initrd_path, 64);
    ensure!(
        out.len() == MANIFEST_SIZE,
        "internal manifest size mismatch: {}",
        out.len()
    );
    Ok(out)
}

fn compile_cpu_features(features: Option<&CpuFeatures>) -> Result<(u8, u16, u16)> {
    let Some(features) = features else {
        return Ok((CPU_FEATURES_VERSION, 0, 0));
    };
    let feature_mask = |names: &[String]| -> Result<u16> {
        let mut mask = 0u16;
        for name in names {
            let bit = match name.as_str() {
                "fp" => 1u16 << 0,
                "simd" => 1u16 << 1,
                "crypto" => 1u16 << 2,
                "rng" => 1u16 << 3,
                "vector" => 1u16 << 4,
                "nested-virt" => 1u16 << 5,
                _ => anyhow::bail!("unsupported CPU feature {name:?}"),
            };
            mask |= bit;
        }
        Ok(mask)
    };
    Ok((
        features.version.unwrap_or(CPU_FEATURES_VERSION),
        feature_mask(&features.required)?,
        feature_mask(&features.prohibited)?,
    ))
}

fn artifact_hash(artifact: &Artifact, placement: &Placement, name: &str) -> Result<[u8; 32]> {
    let override_hash = match name {
        "kernel" => placement.kernel_sha256.as_ref(),
        "dtb" => placement.dtb_sha256.as_ref(),
        "initrd" => placement.initrd_sha256.as_ref(),
        _ => None,
    };
    parse_hash(
        override_hash
            .or(artifact.sha256.as_ref())
            .with_context(|| format!("artifacts.{name}.sha256 is required"))?,
    )
}

fn required<'a, T>(value: &'a Option<T>, name: &str) -> Result<&'a T> {
    value
        .as_ref()
        .with_context(|| format!("{name} is required"))
}

fn enum_value(value: &str, choices: &[&str]) -> Result<()> {
    ensure!(choices.contains(&value), "unsupported value {value:?}");
    Ok(())
}

fn map_enum(value: &str, choices: &[&str]) -> Result<u8> {
    choices
        .iter()
        .position(|v| *v == value)
        .map(|n| n as u8 + 1)
        .with_context(|| format!("unsupported enum {value:?}"))
}

fn parse_hash(value: &str) -> Result<[u8; 32]> {
    ensure!(
        value.len() == 64 && value.bytes().all(|b| b.is_ascii_hexdigit()),
        "SHA-256 must be exactly 64 hexadecimal characters"
    );
    let mut out = [0u8; 32];
    for (index, byte) in out.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&value[index * 2..index * 2 + 2], 16)?;
    }
    ensure!(
        out.iter().any(|b| *b != 0),
        "SHA-256 identity cannot be all zeroes"
    );
    Ok(out)
}

fn push_u16(out: &mut Vec<u8>, value: u16) {
    out.extend_from_slice(&value.to_le_bytes());
}
fn push_u32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}
fn push_u64(out: &mut Vec<u8>, value: u64) {
    out.extend_from_slice(&value.to_le_bytes());
}
fn push_text(out: &mut Vec<u8>, value: &str, width: usize) {
    out.extend_from_slice(value.as_bytes());
    out.resize(out.len() + width - value.len(), 0);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn x86_secondary_manifest_requires_independent_slot_identity() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guest-profiles");
        let (primary, _) = resolve(&root, Path::new("debian-amd64.toml"), &mut Vec::new()).unwrap();
        let (secondary, canonical) = resolve(
            &root,
            Path::new("debian-amd64-secondary.toml"),
            &mut Vec::new(),
        )
        .unwrap();
        validate_x86_slot_profile(&secondary, X86BuildSlot::Secondary).unwrap();
        assert!(validate_x86_slot_profile(&secondary, X86BuildSlot::Primary).is_err());
        assert!(validate_x86_slot_profile(&primary, X86BuildSlot::Secondary).is_err());
        for field in 0..4 {
            let mut mixed = secondary.clone();
            let target = mixed.target.as_mut().unwrap();
            match field {
                0 => target.guest_id = Some(0),
                1 => target.control_type = Some(1),
                2 => target.network_client = Some(0),
                _ => target.block_media = Some(0),
            }
            assert!(validate_x86_slot_profile(&mixed, X86BuildSlot::Secondary).is_err());
        }
        let manifest = compile(&secondary, &canonical, "default").unwrap();
        assert_eq!(manifest.len(), MANIFEST_SIZE);
        assert_eq!(&manifest[16..20], &1u32.to_le_bytes());
        assert_eq!(&manifest[28..30], &1u16.to_le_bytes());
        assert_eq!(&manifest[30..32], &1u16.to_le_bytes());
        assert_eq!(&manifest[244..248], &2u32.to_le_bytes());
    }

    #[test]
    fn x86_boot_selection_rejects_unsupported_resource_requests() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guest-profiles");
        let (profile, _) = resolve(&root, Path::new("debian-amd64.toml"), &mut Vec::new()).unwrap();
        validate_x86_boot_profile(&profile).unwrap();
        let mut bad = profile.clone();
        bad.target.as_mut().unwrap().vcpus = Some(2);
        validate_x86_boot_profile(&bad).unwrap();
        bad.target.as_mut().unwrap().vcpus = Some(3);
        assert!(validate_x86_boot_profile(&bad).is_err());
        let mut bad = profile.clone();
        bad.target.as_mut().unwrap().guest_id = Some(1);
        assert!(validate_x86_boot_profile(&bad).is_err());
        let mut bad = profile.clone();
        bad.target.as_mut().unwrap().autostart = Some(false);
        assert!(validate_x86_boot_profile(&bad).is_err());
        let mut bad = profile.clone();
        bad.placements.get_mut("default").unwrap().ram_size = Some(0x80200000);
        assert!(validate_x86_boot_profile(&bad).is_err());
        let mut bad = profile.clone();
        bad.placements.get_mut("default").unwrap().vmm_hva_base = Some(0x40000000);
        assert!(validate_x86_boot_profile(&bad).is_err());
        let mut bad = profile.clone();
        bad.placements
            .get_mut("default")
            .unwrap()
            .kernel_entry_address = Some(0x200000);
        assert!(validate_x86_boot_profile(&bad).is_err());
        let mut bad = profile;
        bad.target.as_mut().unwrap().network_client = Some(1);
        assert!(validate_x86_boot_profile(&bad).is_err());
    }

    #[test]
    fn arch_installer_retains_and_preloads_live_root_module_closure() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guest-profiles");
        let (profile, _) = resolve(
            &root,
            Path::new("arch-amd64-installer.toml"),
            &mut Vec::new(),
        )
        .unwrap();
        validate_x86_boot_profile(&profile).unwrap();

        let command_line = profile
            .boot
            .as_ref()
            .and_then(|boot| boot.command_line.as_deref())
            .unwrap();
        assert!(command_line
            .split_ascii_whitespace()
            .any(|arg| arg == "earlymodules=virtio_mmio,loop,squashfs,overlay"));

        let modules = profile
            .host
            .as_ref()
            .unwrap()
            .acquire
            .iter()
            .find(|step| step.action == "filter-initramfs-modules")
            .and_then(|step| step.args.get("modules"))
            .unwrap()
            .split(',')
            .collect::<std::collections::BTreeSet<_>>();
        for required in [
            "virtio_mmio",
            "virtio_net",
            "net_failover",
            "failover",
            "loop",
            "squashfs",
            "overlay",
        ] {
            assert!(modules.contains(required), "missing {required}");
        }
    }

    #[test]
    fn x86_cpu_requests_match_fixed_target_exposure() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guest-profiles");
        let (mut profile, _) =
            resolve(&root, Path::new("debian-amd64-2g.toml"), &mut Vec::new()).unwrap();
        validate_x86_boot_profile(&profile).unwrap();
        let features = profile
            .target
            .as_ref()
            .unwrap()
            .cpu_features
            .as_ref()
            .unwrap();
        assert_eq!(features.required, ["fp", "simd"]);
        assert_eq!(
            features.prohibited,
            ["crypto", "rng", "vector", "nested-virt"]
        );
        for feature in ["fp", "simd", "crypto", "rng", "vector", "nested-virt"] {
            let exposed = ["fp", "simd"].contains(&feature);
            profile.target.as_mut().unwrap().cpu_features = Some(CpuFeatures {
                version: Some(CPU_FEATURES_VERSION),
                required: vec![feature.into()],
                prohibited: vec![],
            });
            assert_eq!(validate_x86_boot_profile(&profile).is_ok(), exposed);
            profile.target.as_mut().unwrap().cpu_features = Some(CpuFeatures {
                version: Some(CPU_FEATURES_VERSION),
                required: vec![],
                prohibited: vec![feature.into()],
            });
            assert_eq!(validate_x86_boot_profile(&profile).is_ok(), !exposed);
        }
    }

    #[test]
    fn x86_boot_selection_accepts_second_ram_gib_and_rejects_misalignment() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guest-profiles");
        let (mut profile, _) =
            resolve(&root, Path::new("debian-amd64-2g.toml"), &mut Vec::new()).unwrap();
        assert_eq!(profile.placements["default"].ram_size, Some(0x80000000));
        for ram in [0x40200000, 0x60000000, 0x80000000] {
            profile.placements.get_mut("default").unwrap().ram_size = Some(ram);
            validate_x86_boot_profile(&profile).unwrap();
        }
        profile.placements.get_mut("default").unwrap().ram_size = Some(0x80000001);
        assert!(validate_x86_boot_profile(&profile).is_err());
        profile.placements.get_mut("default").unwrap().ram_size = Some(0x60001000);
        assert!(validate_x86_boot_profile(&profile).is_err());
    }

    #[test]
    fn uefi_acquisition_has_no_fdt_template() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guest-profiles");
        let (mut profile, _) =
            resolve(&root, Path::new("debian-amd64.toml"), &mut Vec::new()).unwrap();
        validate(&profile, None).unwrap();
        profile
            .host
            .as_mut()
            .unwrap()
            .build
            .as_mut()
            .unwrap()
            .template = Some("fake.dts".into());
        assert!(validate(&profile, None).is_err());
        profile
            .host
            .as_mut()
            .unwrap()
            .build
            .as_mut()
            .unwrap()
            .template = None;
        profile.target.as_mut().unwrap().boot_protocol = Some("fdt-direct".into());
        assert!(validate(&profile, None)
            .unwrap_err()
            .to_string()
            .contains("uefi-artifacts requires"));
    }

    #[test]
    fn uefi_is_not_silently_prepared_as_an_fdt_bundle() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guest-profiles");
        let (_, canonical) =
            resolve(&root, Path::new("ubuntu-live.toml"), &mut Vec::new()).unwrap();
        let mut source: toml::Value = toml::from_str(&canonical).unwrap();
        source["target"]["boot_protocol"] = toml::Value::String("uefi".into());
        let temp = tempfile::tempdir().unwrap();
        fs::write(
            temp.path().join("uefi.toml"),
            toml::to_string(&source).unwrap(),
        )
        .unwrap();
        let output = temp.path().join("bundle");
        let error = prepare_bundle(
            temp.path(),
            Path::new("uefi.toml"),
            "default",
            temp.path(),
            &output,
        )
        .unwrap_err();
        assert!(
            error
                .to_string()
                .contains("require the fdt-direct boot protocol"),
            "{error:#}"
        );
        assert!(!output.exists());
    }

    #[test]
    fn uefi_without_dtb_has_canonical_absent_fields() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guest-profiles");
        let (mut profile, _) =
            resolve(&root, Path::new("ubuntu-live.toml"), &mut Vec::new()).unwrap();
        let target = profile.target.as_mut().unwrap();
        target.architecture = Some("x86-64".into());
        target.boot_protocol = Some("uefi".into());
        target.kernel_format = Some("uefi".into());
        target.entry_from_image = Some(false);
        profile.artifacts.remove("dtb");
        for p in profile.placements.values_mut() {
            p.dtb_load_address = None;
            p.dtb_sha256 = None;
            p.kernel_entry_address = p.kernel_load_address;
        }
        validate(&profile, Some("default")).unwrap();
        let manifest = compile(&profile, "uefi-without-dtb-test", "default").unwrap();
        assert_eq!(manifest.len(), MANIFEST_SIZE);
        assert_eq!(&manifest[72..80], &[0; 8]); // DTB load address
        assert_eq!(&manifest[96..104], &[0; 8]); // DTB maximum bytes
        assert_eq!(&manifest[176..208], &[0; 32]); // DTB hash
        profile
            .placements
            .get_mut("default")
            .unwrap()
            .dtb_load_address = Some(0x4000_0000);
        assert!(validate(&profile, None).is_err());
        profile
            .placements
            .get_mut("default")
            .unwrap()
            .dtb_load_address = None;
        profile.placements.get_mut("default").unwrap().dtb_sha256 = Some("a5".repeat(32));
        assert!(validate(&profile, None).is_err());
        profile.placements.get_mut("default").unwrap().dtb_sha256 = None;
        profile.target.as_mut().unwrap().boot_protocol = Some("fdt-direct".into());
        assert!(validate(&profile, None).is_err());
    }

    #[test]
    fn merge_replaces_scalars_and_preserves_tables() {
        let mut base: toml::Value = toml::from_str("[a]\nx=1\ny=2\n").unwrap();
        let child: toml::Value = toml::from_str("[a]\nx=3\n").unwrap();
        merge(&mut base, child);
        assert_eq!(base["a"]["x"].as_integer(), Some(3));
        assert_eq!(base["a"]["y"].as_integer(), Some(2));
    }

    #[test]
    fn paths_cannot_escape_profile_root() {
        assert!(safe_relative(Path::new("../outside.toml")).is_err());
        assert!(safe_relative(Path::new("/outside.toml")).is_err());
        assert!(safe_relative(Path::new("linux/buildroot.toml")).is_ok());
    }

    #[test]
    fn hashes_are_exact_and_nonzero() {
        assert!(parse_hash(&"a5".repeat(32)).is_ok());
        assert!(parse_hash(&"00".repeat(32)).is_err());
        assert!(parse_hash("1234").is_err());
    }

    #[test]
    fn cpu_features_and_selected_resources_fail_closed() {
        let valid_features = CpuFeatures {
            version: Some(CPU_FEATURES_VERSION),
            required: vec!["fp".to_string(), "simd".to_string()],
            prohibited: vec!["nested-virt".to_string()],
        };
        assert!(validate_cpu_features(Some(&valid_features)).is_ok());
        let mut conflicting = valid_features.clone();
        conflicting.prohibited.push("fp".to_string());
        assert!(validate_cpu_features(Some(&conflicting)).is_err());
        let mut unknown_version = valid_features;
        unknown_version.version = Some(CPU_FEATURES_VERSION + 1);
        assert!(validate_cpu_features(Some(&unknown_version)).is_err());

        let first = Placement {
            guest_gpa_base: Some(0x4000_0000),
            vmm_hva_base: Some(0x8000_0000),
            ram_size: Some(0x2000_0000),
            ..Placement::default()
        };
        let second = Placement {
            guest_gpa_base: Some(0x6000_0000),
            vmm_hva_base: Some(0xa000_0000),
            ram_size: Some(0x2000_0000),
            ..Placement::default()
        };
        assert!(validate_selected_placements([("first", &first), ("second", &second)]).is_ok());
        let gpa_overlap = Placement {
            guest_gpa_base: Some(0x5fff_f000),
            ..second.clone()
        };
        assert!(
            validate_selected_placements([("first", &first), ("second", &gpa_overlap)]).is_err()
        );
        let hva_overlap = Placement {
            vmm_hva_base: Some(0x9fff_f000),
            ..second
        };
        assert!(
            validate_selected_placements([("first", &first), ("second", &hva_overlap)]).is_err()
        );

        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guest-profiles");
        let (mut desktop, _) =
            resolve(&root, Path::new("ubuntu-live.toml"), &mut Vec::new()).unwrap();
        desktop.placements.get_mut("default").unwrap().ram_size = Some(RAM_MAX);
        assert!(validate(&desktop, Some("default")).is_ok());
        let desktop_placement = desktop.placements.get("default").unwrap();
        let overflow = Placement {
            guest_gpa_base: Some(0x2_4000_0000),
            vmm_hva_base: Some(0x2_8000_0000),
            ram_size: Some(RAM_ALIGN),
            ..Placement::default()
        };
        assert!(validate_selected_placements([
            ("desktop", desktop_placement),
            ("small", &overflow),
        ])
        .is_err());
    }

    #[test]
    fn inheritance_cycles_fail_closed() {
        let root = tempfile::tempdir().unwrap();
        fs::write(root.path().join("a.toml"), "extends='b.toml'\n").unwrap();
        fs::write(root.path().join("b.toml"), "extends='a.toml'\n").unwrap();
        assert!(resolve(root.path(), Path::new("a.toml"), &mut Vec::new()).is_err());
    }

    #[test]
    fn unknown_fields_fail_closed() {
        let root = tempfile::tempdir().unwrap();
        fs::write(
            root.path().join("bad.toml"),
            "schema=1\nid='bad'\nstatus='abstract'\nsurprise=true\n",
        )
        .unwrap();
        assert!(resolve(root.path(), Path::new("bad.toml"), &mut Vec::new()).is_err());
    }

    #[test]
    fn nocloud_seed_contract_binds_sources_and_provisioning() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR"))
            .parent()
            .unwrap()
            .join("guest-profiles");
        let path = Path::new("debian-arm64-nocloud.toml");
        let (profile, _) = resolve(&root, path, &mut Vec::new()).unwrap();
        let host = profile.host.unwrap();
        assert!(validate_host(Some(&host)).is_ok());
        let seed = host_profile_plan(&root, path).unwrap().seed.unwrap();
        assert_eq!(seed.partition_offset, 134217728);
        assert_eq!(
            seed.root_ext4,
            "_build/guest-images/debian-arm64-nocloud/root.ext4"
        );
        for bad_path in ["", "/tmp/root.ext4", "../root.ext4", "_build/../root.ext4"] {
            let mut invalid = host.clone();
            invalid.seed.as_mut().unwrap().root_ext4 = bad_path.into();
            assert!(validate_host(Some(&invalid)).is_err(), "{bad_path}");
        }
        let mut invalid = host.clone();
        invalid.seed.as_mut().unwrap().partition_offset = 513;
        assert!(validate_host(Some(&invalid)).is_err());
        let mut invalid = host.clone();
        invalid.seed.as_mut().unwrap().adapter = "unknown".into();
        assert!(validate_host(Some(&invalid)).is_err());
        let mut invalid = host.clone();
        invalid.qemu.as_mut().unwrap().ssh.as_mut().unwrap().account = "root".into();
        assert!(validate_host(Some(&invalid)).is_err());
        let mut invalid = host.clone();
        invalid.qemu.as_mut().unwrap().media.clear();
        assert!(validate_host(Some(&invalid)).is_err());
        let mut invalid = host;
        invalid.provision.push(RecipeStep {
            action: "write-console".into(),
            args: BTreeMap::new(),
        });
        assert!(validate_host(Some(&invalid)).is_err());
    }

    #[test]
    fn host_recipes_are_bounded() {
        let steps = (0..=MAX_RECIPE_STEPS)
            .map(|_| RecipeStep {
                action: "copy".to_string(),
                args: BTreeMap::new(),
            })
            .collect();
        let profile = Profile {
            schema: Some(VERSION),
            id: Some("bounded".to_string()),
            status: Some(Status::Abstract),
            host: Some(Host {
                seed: None,
                qemu: None,
                console: None,
                desktop: None,
                build: None,
                acquire: steps,
                provision: Vec::new(),
                test: Vec::new(),
            }),
            ..Profile::default()
        };
        assert!(validate(&profile, None).is_err());
    }

    #[test]
    fn initramfs_binary_recipe_requires_one_payload_and_a_pin() {
        for action in ["build-initramfs-file", "append-initramfs-file"] {
            let mut step = RecipeStep {
                action: action.into(),
                args: BTreeMap::from([
                    ("output".into(), "initrd".into()),
                    ("path".into(), "init".into()),
                    ("mode".into(), "0755".into()),
                    ("content_file".into(), "helper".into()),
                    ("content_sha256".into(), "ab".repeat(32)),
                ]),
            };
            if action == "append-initramfs-file" {
                step.args.insert("source".into(), "base".into());
            }
            assert!(validate_host_action(&step).is_ok());
            step.args.insert("content_file".into(), "../helper".into());
            assert!(validate_host_action(&step).is_err());
            step.args.insert("content_file".into(), "helper".into());
            step.args.insert("content".into(), "text".into());
            assert!(validate_host_action(&step).is_err());
            step.args.remove("content");
            step.args.remove("content_sha256");
            assert!(validate_host_action(&step).is_err());
            step.args.insert("content_sha256".into(), "invalid".into());
            assert!(validate_host_action(&step).is_err());
            step.args.remove("content_file");
            step.args.insert("content".into(), "text".into());
            assert!(validate_host_action(&step).is_err());
            step.args.remove("content_sha256");
            assert!(validate_host_action(&step).is_ok());
        }
    }

    #[test]
    fn host_action_arguments_fail_closed() {
        let valid = RecipeStep {
            action: "stage-url".to_string(),
            args: BTreeMap::from([
                ("cache_name".to_string(), "guest.iso".to_string()),
                ("output".to_string(), "guest.iso".to_string()),
                (
                    "url".to_string(),
                    "https://example.invalid/guest.iso".to_string(),
                ),
            ]),
        };
        assert!(validate_host_action(&valid).is_ok());

        let mut sha256 = valid.clone();
        sha256.args.insert("sha256".to_string(), "a".repeat(64));
        assert!(validate_host_action(&sha256).is_ok());
        sha256.args.insert("sha256".to_string(), "0".repeat(64));
        assert!(validate_host_action(&sha256).is_err());

        let mut typo = valid.clone();
        typo.args
            .insert("cache_nam".to_string(), "guest.iso".to_string());
        assert!(validate_host_action(&typo).is_err());

        let mut insecure = valid;
        insecure.args.insert(
            "url".to_string(),
            "http://example.invalid/guest.iso".to_string(),
        );
        assert!(validate_host_action(&insecure).is_err());
    }

    #[test]
    fn console_expect_dsl_is_bounded_and_fail_closed() {
        let console = HostConsole {
            adapter: "expect".to_string(),
            success: vec!["# ".to_string()],
            require: Vec::new(),
            reject: vec!["mountroot>".to_string()],
            interaction: vec![ConsoleInteraction {
                when: vec!["login:".to_string()],
                send: "root\r".to_string(),
                max_fires: 4,
                after_secs: None,
            }],
            probe_line: Some("printf 'ready-%s\\n' proof".to_string()),
            probe_marker: Some("ready-proof".to_string()),
            probe_timeout_secs: Some(30),
        };
        let host = Host {
            seed: None,
            qemu: None,
            console: Some(console.clone()),
            desktop: None,
            build: None,
            acquire: Vec::new(),
            provision: Vec::new(),
            test: Vec::new(),
        };
        assert!(validate_host(Some(&host)).is_ok());

        let mut unbounded = console.clone();
        unbounded.interaction[0].max_fires = 0;
        let mut invalid = host.clone();
        invalid.console = Some(unbounded);
        assert!(validate_host(Some(&invalid)).is_err());

        let mut named_adapter = console;
        named_adapter.adapter = "installer-shell".to_string();
        invalid.console = Some(named_adapter);
        assert!(validate_host(Some(&invalid)).is_err());
    }

    #[test]
    fn aliases_resolve_to_bounded_host_plans() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guest-profiles");
        let path = resolve_alias(&root, "ubuntu-live").unwrap();
        assert_eq!(path, PathBuf::from("ubuntu-live.toml"));
        let plan = host_profile_plan(&root, &path).unwrap();
        assert_eq!(plan.id, "ubuntu-live-aarch64");
        assert_eq!(plan.qemu.as_ref().unwrap().memory, "3G");
        assert_eq!(plan.qemu.as_ref().unwrap().media[0].bus, 8);
        assert!(plan.test.iter().any(|step| step.action == "assert-virtio"
            && step.args.get("scope").map(String::as_str) == Some("host-backed")));
        assert_eq!(plan.desktop.as_ref().unwrap().local_port, 15901);

        let (_, acquire) = acquire_recipe(&root, &path).unwrap();
        assert_eq!(acquire.first().unwrap().action, "stage-url");
        assert!(acquire
            .iter()
            .any(|step| step.action == "build-initramfs-file"));
        assert_eq!(acquire.last().unwrap().action, "extract-arm64-linux-image");
    }

    #[test]
    fn nocloud_accepts_a_distinct_guest_address_and_rejects_service_addresses() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guest-profiles");
        let (mut profile, _) = resolve(
            &root,
            Path::new("debian-arm64-nocloud.toml"),
            &mut Vec::new(),
        )
        .unwrap();
        let host = profile.host.as_mut().unwrap();
        host.qemu
            .as_mut()
            .unwrap()
            .ssh
            .as_mut()
            .unwrap()
            .guest_address = Some("10.0.2.16".into());
        assert!(validate_host(Some(host)).is_ok());
        host.qemu
            .as_mut()
            .unwrap()
            .ssh
            .as_mut()
            .unwrap()
            .guest_address = Some("10.0.2.2".into());
        assert!(validate_host(Some(host)).is_err());
        host.qemu
            .as_mut()
            .unwrap()
            .ssh
            .as_mut()
            .unwrap()
            .guest_address = None;
        assert!(validate_host(Some(host)).is_err());
    }

    #[test]
    fn seeded_graphics_inherits_provisioning_and_bounds_ssh_assertions() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guest-profiles");
        let path = resolve_alias(&root, "debian-nocloud-graphics-input").unwrap();
        let plan = host_profile_plan(&root, &path).unwrap();
        assert!(plan.seed.is_some());
        assert!(plan.provision.is_empty());
        assert!(plan.console.interaction.is_empty());
        assert_eq!(
            plan.qemu.as_ref().unwrap().ssh.as_ref().unwrap().account,
            "debian"
        );
        let mut assertion = plan
            .test
            .iter()
            .find(|s| s.action == "assert-ssh-output")
            .unwrap()
            .clone();
        assert!(validate_host_action(&assertion).is_ok());
        assertion.args.insert("command".into(), "x".repeat(4097));
        assert!(validate_host_action(&assertion).is_err());
        assertion
            .args
            .insert("command".into(), "bad\0command".into());
        assert!(validate_host_action(&assertion).is_err());
        assertion.args.remove("stdout");
        assert!(validate_host_action(&assertion).is_err());
    }

    #[test]
    fn desktop_profile_policy_is_bounded_and_fail_closed() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guest-profiles");
        let (mut profile, _) = resolve(&root, Path::new("ubuntu-live.toml"), &mut Vec::new())
            .expect("resolve desktop profile");
        assert!(validate(&profile, None).is_ok());

        profile
            .host
            .as_mut()
            .unwrap()
            .desktop
            .as_mut()
            .unwrap()
            .local_port = 0;
        assert!(validate(&profile, None).is_err());

        let desktop = profile.host.as_mut().unwrap().desktop.as_mut().unwrap();
        desktop.local_port = 15901;
        desktop.guest_port = Some(5901);
        assert!(validate(&profile, None).is_err());

        let desktop = profile.host.as_mut().unwrap().desktop.as_mut().unwrap();
        desktop.guest_port = None;
        desktop.guest_socket = Some("/tmp/../escape.sock".to_string());
        assert!(validate(&profile, None).is_err());
    }

    #[test]
    fn media_initrd_exact_size_requires_a_bounded_cache_path() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guest-profiles");
        let (mut profile, _) = resolve(&root, Path::new("ubuntu-live.toml"), &mut Vec::new())
            .expect("resolve live profile");
        assert!(validate(&profile, None).is_ok());

        profile
            .host
            .as_mut()
            .unwrap()
            .build
            .as_mut()
            .unwrap()
            .media_initrd_cache = None;
        assert!(validate(&profile, None).is_err());
    }

    #[test]
    fn planned_profiles_cannot_execute_on_the_host() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../guest-profiles");
        let path = resolve_alias(&root, "omarchy").unwrap();
        assert!(host_profile_plan(&root, &path).is_err());
    }
}
