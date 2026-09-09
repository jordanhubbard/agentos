//! Compile bounded guest profile TOML into the target manifest wire format.

use anyhow::{ensure, Context, Result};
use clap::Args;
use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::io::Read;
use std::path::{Component, Path, PathBuf};

const MAGIC: u64 = 0x0046_5250_4753_4f41;
const VERSION: u16 = 2;
const MANIFEST_SIZE: usize = 640;
const MAX_INHERITANCE_DEPTH: usize = 8;
const MAX_RECIPE_STEPS: usize = 64;

#[derive(Args)]
pub struct GuestProfileArgs {
    /// Directory containing profile TOML files.
    #[arg(long, default_value = "guest-profiles")]
    pub root: PathBuf,
    /// Profile path, relative to --root.
    #[arg(long)]
    pub profile: Option<PathBuf>,
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
    devices: Option<Vec<String>>,
    network_client: Option<u16>,
    block_media: Option<u16>,
    autostart: Option<bool>,
    entry_from_image: Option<bool>,
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

#[derive(Clone, Debug, Deserialize)]
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
    #[serde(default)]
    acquire: Vec<RecipeStep>,
    #[serde(default)]
    provision: Vec<RecipeStep>,
    #[serde(default)]
    test: Vec<RecipeStep>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct RecipeStep {
    pub(crate) action: String,
    #[serde(default)]
    pub(crate) args: BTreeMap<String, String>,
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
    "decompress-gzip",
    "copy",
    "build-initramfs",
    "build-linux-probe-initramfs",
    "wait-console",
    "send-console",
    "wait-ssh",
    "run-ssh",
    "assert-console",
    "assert-virtio",
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
                    | "build-linux-probe-initramfs"
            ),
            "runtime host.acquire action {:?} has no bounded executor",
            step.action
        );
    }
    Ok((id, steps))
}

pub fn run(args: &GuestProfileArgs) -> Result<()> {
    ensure!(
        args.root.is_dir(),
        "profile root does not exist: {}",
        args.root.display()
    );
    if args.check_all {
        ensure!(
            args.profile.is_none(),
            "--check-all and --profile are mutually exclusive"
        );
        ensure!(args.output.is_none(), "--check-all does not emit --output");
        ensure!(
            !args.verify_artifacts,
            "--check-all cannot verify artifacts for mutually exclusive profiles"
        );
        let files = profile_files(&args.root)?;
        ensure!(
            !files.is_empty(),
            "no TOML profiles under {}",
            args.root.display()
        );
        for path in &files {
            let (profile, _) = resolve(&args.root, path, &mut Vec::new())?;
            validate(&profile, None)
                .with_context(|| format!("invalid profile {}", path.display()))?;
        }
        println!("[guest-profile] validated {} profiles", files.len());
        return Ok(());
    }

    let profile_path = args
        .profile
        .as_ref()
        .context("--profile is required unless --check-all is used")?;
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

fn validate(profile: &Profile, placement: Option<&str>) -> Result<()> {
    ensure!(profile.schema == Some(VERSION), "schema must be {VERSION}");
    let id = required(&profile.id, "id")?;
    ensure!(
        !id.is_empty() && id.len() <= 63 && id.is_ascii(),
        "id must be 1..63 ASCII bytes"
    );
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
        target.vcpus.is_some_and(|n| (1..=8).contains(&n)),
        "target.vcpus must be 1..8"
    );
    ensure!(
        target.control_type.is_some_and(|value| value != 0),
        "target.control_type must be nonzero"
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
        cmdline.len() <= 255 && cmdline.is_ascii(),
        "boot.command_line must be at most 255 ASCII bytes"
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

    for name in ["kernel", "dtb"] {
        validate_artifact(profile, name, status == Status::Runtime)?;
    }
    if profile.artifacts.contains_key("initrd") {
        validate_artifact(profile, "initrd", status == Status::Runtime)?;
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
        validate_placement(name, value)?;
        validate_artifact_windows(profile, name, value)?;
    }
    if let Some(name) = placement {
        ensure!(
            profile.placements.contains_key(name),
            "placement {name:?} is not defined"
        );
    }
    Ok(())
}

fn validate_artifact_windows(profile: &Profile, name: &str, p: &Placement) -> Result<()> {
    let kernel_address = p.kernel_load_address.unwrap();
    let dtb_address = p.dtb_load_address.unwrap();
    let kernel_size = profile.artifacts["kernel"].max_bytes.unwrap();
    let dtb_size = profile.artifacts["dtb"].max_bytes.unwrap();
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

fn validate_placement(name: &str, p: &Placement) -> Result<()> {
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
        ram >= 0x20_0000 && ram & 0x1f_ffff == 0,
        "placement RAM must be >=2 MiB and 2 MiB aligned"
    );
    ensure!(
        gpa.checked_add(ram).is_some() && hva.checked_add(ram).is_some(),
        "placement address range wraps"
    );
    for (field, address) in [
        ("kernel_load_address", p.kernel_load_address),
        ("dtb_load_address", p.dtb_load_address),
    ] {
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

fn validate_host_action(step: &RecipeStep) -> Result<()> {
    let (required_args, optional_args): (&[&str], &[&str]) = match step.action.as_str() {
        "stage-url" => (&["cache_name", "output", "url"], &["override_env"]),
        "download-tar-member" => (&["url", "member", "output"], &[]),
        "extract-iso-file" => (&["source", "member", "output"], &["min_bytes"]),
        "extract-arm64-linux-image" | "extract-arm64-elf-image" => {
            (&["source", "member", "output"], &[])
        }
        "build-linux-probe-initramfs" => (&["output"], &[]),
        "download" | "verify-sha256" => (&["artifact"], &[]),
        "extract-ufs-file" => (&["member", "artifact"], &[]),
        "decompress-gzip" | "copy" => (&["source", "output"], &[]),
        "build-initramfs" => (&["recipe", "artifact"], &[]),
        "wait-console" | "assert-console" => (&["marker"], &[]),
        "send-console" => (&["text"], &[]),
        "wait-ssh" => (&["account"], &[]),
        "run-ssh" => (&["recipe"], &[]),
        "assert-virtio" => (&["devices"], &[]),
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
    if matches!(step.action.as_str(), "stage-url" | "download-tar-member") {
        ensure!(
            step.args["url"].starts_with("https://"),
            "host downloads must use HTTPS"
        );
    }
    if let Some(value) = step.args.get("min_bytes") {
        ensure!(
            value.parse::<u64>().is_ok_and(|number| number > 0),
            "min_bytes must be a positive integer"
        );
    }
    Ok(())
}

fn compile(profile: &Profile, canonical: &str, placement_name: &str) -> Result<Vec<u8>> {
    let target = profile.target.as_ref().unwrap();
    let placement = &profile.placements[placement_name];
    let kernel = &profile.artifacts["kernel"];
    let dtb = &profile.artifacts["dtb"];
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
        placement.dtb_load_address.unwrap(),
        placement.initrd_load_address.unwrap_or(0),
        kernel.max_bytes.unwrap(),
        dtb.max_bytes.unwrap(),
        initrd.and_then(|a| a.max_bytes).unwrap_or(0),
    ] {
        push_u64(&mut out, value);
    }
    out.extend_from_slice(&Sha256::digest(canonical.as_bytes()));
    out.extend_from_slice(&artifact_hash(kernel, placement, "kernel")?);
    out.extend_from_slice(&artifact_hash(dtb, placement, "dtb")?);
    if let Some(initrd) = initrd {
        out.extend_from_slice(&artifact_hash(initrd, placement, "initrd")?);
    } else {
        out.extend_from_slice(&[0; 32]);
    }
    push_u16(&mut out, command_line.len() as u16);
    push_u16(&mut out, id.len() as u16);
    push_u32(&mut out, target.control_type.unwrap());
    push_u16(&mut out, media_initrd_path.len() as u16);
    out.extend_from_slice(&[0; 6]);
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
                acquire: steps,
                provision: Vec::new(),
                test: Vec::new(),
            }),
            ..Profile::default()
        };
        assert!(validate(&profile, None).is_err());
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
}
