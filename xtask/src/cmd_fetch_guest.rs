use crate::cmd_guest_profile::{self, RecipeStep};
use crate::FetchGuestArgs;
use anyhow::Context;
use sha2::{Digest, Sha256, Sha512};
use std::ffi::OsString;
use std::fs::{self, OpenOptions};
use std::io::{ErrorKind, Read, Seek, SeekFrom, Write};
use std::path::{Component, Path, PathBuf};
use std::process::Stdio;
use std::time::UNIX_EPOCH;

const ISO_DIR_ENV: &str = "AGENTOS_ISO_DIR";
const COPY_ISOS_ENV: &str = "AGENTOS_COPY_ISOS";

fn repo_root() -> anyhow::Result<PathBuf> {
    let out = std::process::Command::new("git")
        .args(["rev-parse", "--show-toplevel"])
        .output()
        .context("failed to run git rev-parse")?;
    anyhow::ensure!(out.status.success(), "not in a git repository");
    let root = String::from_utf8(out.stdout)
        .context("git output not utf-8")?
        .trim()
        .to_string();
    Ok(PathBuf::from(root))
}

fn build_tmp_dir() -> anyhow::Result<PathBuf> {
    let dir = repo_root()?.join("_build/tmp");
    fs::create_dir_all(&dir).with_context(|| format!("failed to create {}", dir.display()))?;
    Ok(dir)
}

fn iso_dir_from_env(
    agentos_iso_dir: Option<OsString>,
    xdg_cache_home: Option<OsString>,
    home: Option<OsString>,
) -> PathBuf {
    if let Some(d) = agentos_iso_dir {
        return PathBuf::from(d);
    }
    let cache_root = xdg_cache_home
        .map(PathBuf::from)
        .or_else(|| home.map(|h| PathBuf::from(h).join(".cache")))
        .unwrap_or_else(|| PathBuf::from("/tmp"));
    cache_root.join("agentos").join("isos")
}

fn iso_dir() -> PathBuf {
    iso_dir_from_env(
        std::env::var_os(ISO_DIR_ENV),
        std::env::var_os("XDG_CACHE_HOME"),
        std::env::var_os("HOME"),
    )
}

fn ensure_cached_iso(iso_name: &str, url: &str) -> anyhow::Result<PathBuf> {
    let cache = iso_dir();
    fs::create_dir_all(&cache)
        .with_context(|| format!("failed to create ISO cache dir: {}", cache.display()))?;
    let cached = cache.join(iso_name);
    if cached.exists() && fs::metadata(&cached).map(|m| m.len()).unwrap_or(0) > 0 {
        return Ok(cached);
    }

    let curl = find_tool(&["curl", "/opt/homebrew/bin/curl", "/usr/bin/curl"])?;
    let tmp = cached.with_extension("part");
    let _ = fs::remove_file(&tmp);
    println!("[fetch-guest] Downloading {} -> {}", url, cached.display());
    let status = std::process::Command::new(&curl)
        .arg("--fail")
        .arg("--location")
        .arg("--progress-bar")
        .arg("--output")
        .arg(&tmp)
        .arg(url)
        .status()
        .with_context(|| format!("failed to run {}", curl.display()))?;
    anyhow::ensure!(status.success(), "ISO download failed: {}", url);
    anyhow::ensure!(
        fs::metadata(&tmp).map(|m| m.len()).unwrap_or(0) > 0,
        "downloaded ISO is empty: {}",
        url
    );
    fs::rename(&tmp, &cached)
        .with_context(|| format!("failed to move {} to {}", tmp.display(), cached.display()))?;
    Ok(cached)
}

fn download_tar_member(url: &str, member: &str, dest: &Path) -> anyhow::Result<()> {
    anyhow::ensure!(
        url.starts_with("https://"),
        "guest recipe downloads must use HTTPS"
    );
    if dest.exists() && fs::metadata(dest).map(|m| m.len()).unwrap_or(0) > 0 {
        println!(
            "[fetch-guest] tar member already staged: {}",
            dest.display()
        );
        return Ok(());
    }
    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("failed to create {}", parent.display()))?;
    }
    let tmp_root = build_tmp_dir()?;
    let tmp_dir = tempfile::Builder::new()
        .prefix("agentos-guest-archive-")
        .tempdir_in(&tmp_root)
        .context("failed to create guest archive tempdir under _build/tmp")?;
    let archive = tmp_dir.path().join("download.tar");
    let curl = find_tool(&["curl", "/opt/homebrew/bin/curl", "/usr/bin/curl"])?;
    let status = std::process::Command::new(&curl)
        .args(["--fail", "--location", "--progress-bar", "--output"])
        .arg(&archive)
        .arg(url)
        .status()
        .with_context(|| format!("failed to run {}", curl.display()))?;
    anyhow::ensure!(status.success(), "archive download failed: {url}");

    let tmp = dest.with_extension("tmp");
    let _ = fs::remove_file(&tmp);
    let out =
        fs::File::create(&tmp).with_context(|| format!("failed to create {}", tmp.display()))?;
    let status = std::process::Command::new("bsdtar")
        .arg("-xOf")
        .arg(&archive)
        .arg(member)
        .stdout(Stdio::from(out))
        .status()
        .context("failed to run bsdtar")?;
    anyhow::ensure!(status.success(), "bsdtar failed extracting {member}");
    anyhow::ensure!(
        fs::metadata(&tmp).map(|m| m.len()).unwrap_or(0) > 0,
        "archive member {member:?} was empty"
    );
    fs::rename(&tmp, dest)
        .with_context(|| format!("failed to move {} to {}", tmp.display(), dest.display()))?;
    Ok(())
}

pub fn run(args: &FetchGuestArgs) -> anyhow::Result<()> {
    let root = repo_root()?;
    let profile_root = if args.profile_root.is_absolute() {
        args.profile_root.clone()
    } else {
        root.join(&args.profile_root)
    };
    let output_dir = match &args.output_dir {
        Some(d) => PathBuf::from(d),
        None => root.join(cmd_guest_profile::acquire_output_dir(
            &profile_root,
            &args.profile,
        )?),
    };
    fs::create_dir_all(&output_dir)
        .with_context(|| format!("failed to create output dir: {}", output_dir.display()))?;

    let (id, recipe) = cmd_guest_profile::acquire_recipe(&profile_root, &args.profile)?;
    for step in &recipe {
        execute_acquire_step(step, &output_dir)?;
    }
    println!(
        "[fetch-guest] profile {id} assets ready under {}",
        output_dir.display()
    );
    Ok(())
}

fn recipe_arg<'a>(step: &'a RecipeStep, key: &str) -> anyhow::Result<&'a str> {
    step.args
        .get(key)
        .map(String::as_str)
        .with_context(|| format!("host action {:?} requires argument {key:?}", step.action))
}

fn recipe_path(output_dir: &Path, value: &str) -> anyhow::Result<PathBuf> {
    let relative = Path::new(value);
    anyhow::ensure!(
        !relative.as_os_str().is_empty()
            && !relative.is_absolute()
            && relative
                .components()
                .all(|c| matches!(c, Component::Normal(_))),
        "guest recipe path must be a confined relative path: {value:?}"
    );
    Ok(output_dir.join(relative))
}

fn execute_acquire_step(step: &RecipeStep, output_dir: &Path) -> anyhow::Result<()> {
    match step.action.as_str() {
        "build-static-linux-elf" => build_static_linux_elf(step, &repo_root()?, output_dir)?,
        "stage-url" => {
            let output = recipe_arg(step, "output")?;
            let dest = recipe_path(output_dir, output)?;
            let override_path =
                source_override(&dest, step.args.get("override_env").map(String::as_str))?;
            if let Some(source) = override_path {
                stage_existing_iso(output_dir, &source, output)?;
            } else {
                stage_local_iso(
                    output_dir,
                    recipe_arg(step, "cache_name")?,
                    output,
                    recipe_arg(step, "url")?,
                )?;
            }
            if let Some(expected) = step.args.get("sha512") {
                verify_sha512(&dest, expected)?;
            }
            if let Some(expected) = step.args.get("sha256") {
                verify_sha256(&dest, expected)?;
            }
        }
        "download-tar-member" => download_tar_member(
            recipe_arg(step, "url")?,
            recipe_arg(step, "member")?,
            &recipe_path(output_dir, recipe_arg(step, "output")?)?,
        )?,
        "extract-iso-file" => {
            let source = recipe_path(output_dir, recipe_arg(step, "source")?)?;
            let output = recipe_path(output_dir, recipe_arg(step, "output")?)?;
            extract_iso_file(&source, recipe_arg(step, "member")?, &output)?;
            if let Some(minimum) = step.args.get("min_bytes") {
                let minimum: u64 = minimum.parse().context("min_bytes must be an integer")?;
                anyhow::ensure!(
                    fs::metadata(&output).map(|m| m.len()).unwrap_or(0) >= minimum,
                    "extracted artifact is smaller than min_bytes: {}",
                    output.display()
                );
            }
        }
        "build-linux-probe-initramfs" => {
            build_linux_probe_initramfs(&recipe_path(output_dir, recipe_arg(step, "output")?)?)?
        }
        "extract-arm64-linux-image" => extract_arm64_linux_image(
            &recipe_path(output_dir, recipe_arg(step, "source")?)?,
            recipe_arg(step, "member")?,
            &recipe_path(output_dir, recipe_arg(step, "output")?)?,
        )?,
        "extract-arm64-elf-image" => extract_arm64_elf_image(
            &recipe_path(output_dir, recipe_arg(step, "source")?)?,
            recipe_arg(step, "member")?,
            &recipe_path(output_dir, recipe_arg(step, "output")?)?,
        )?,
        "build-initramfs-file" => build_initramfs_file(
            &recipe_path(output_dir, recipe_arg(step, "output")?)?,
            recipe_arg(step, "path")?,
            recipe_arg(step, "mode")?,
            &initramfs_payload(step, output_dir)?,
            step.args.get("compression").map(String::as_str),
        )?,
        "append-initramfs-file" => append_initramfs_file(
            &recipe_path(output_dir, recipe_arg(step, "source")?)?,
            &recipe_path(output_dir, recipe_arg(step, "output")?)?,
            recipe_arg(step, "path")?,
            recipe_arg(step, "mode")?,
            &initramfs_payload(step, output_dir)?,
            step.args.get("compression").map(String::as_str),
        )?,
        "filter-initramfs-modules" => filter_initramfs_modules(
            &recipe_path(output_dir, recipe_arg(step, "source")?)?,
            &recipe_path(output_dir, recipe_arg(step, "output")?)?,
            recipe_arg(step, "modules")?,
        )?,
        "convert-qcow2-raw" => convert_qcow2_to_raw(
            &recipe_path(output_dir, recipe_arg(step, "source")?)?,
            &recipe_path(output_dir, recipe_arg(step, "output")?)?,
        )?,
        "extract-gpt-partition" => extract_gpt_partition(
            &recipe_path(output_dir, recipe_arg(step, "source")?)?,
            &recipe_path(output_dir, recipe_arg(step, "output")?)?,
            recipe_arg(step, "index")?,
            step.args.get("sector_size").map(String::as_str),
        )?,
        "extract-ext4-file" => extract_ext4_file(
            &recipe_path(output_dir, recipe_arg(step, "source")?)?,
            &recipe_path(output_dir, recipe_arg(step, "output")?)?,
            recipe_arg(step, "path")?,
        )?,
        "install-gpt-ext4-file" => install_gpt_ext4_file(
            &recipe_path(output_dir, recipe_arg(step, "source")?)?,
            &recipe_path(output_dir, recipe_arg(step, "output")?)?,
            recipe_arg(step, "index")?,
            step.args.get("sector_size").map(String::as_str),
            recipe_arg(step, "path")?,
            recipe_arg(step, "content")?.as_bytes(),
        )?,
        "normalize-arm64-linux-image" => normalize_arm64_linux_image(
            &recipe_path(output_dir, recipe_arg(step, "source")?)?,
            &recipe_path(output_dir, recipe_arg(step, "output")?)?,
        )?,
        other => anyhow::bail!("host acquire action {other:?} is not executable"),
    }
    Ok(())
}

fn gpt_partition_range(
    source: &Path,
    index: &str,
    sector_size: Option<&str>,
) -> anyhow::Result<(u64, u64)> {
    let index = index
        .parse::<u32>()
        .context("GPT partition index must be a positive integer")?;
    anyhow::ensure!(
        (1..=128).contains(&index),
        "GPT partition index must be 1..=128"
    );
    let sector_size = sector_size
        .unwrap_or("512")
        .parse::<u64>()
        .context("GPT sector size must be an integer")?;
    anyhow::ensure!(
        matches!(sector_size, 512 | 4096),
        "GPT sector size must be 512 or 4096"
    );
    let source_len = fs::metadata(source)
        .with_context(|| format!("failed to inspect GPT disk {}", source.display()))?
        .len();
    let mut input = fs::File::open(source)
        .with_context(|| format!("failed to open GPT disk {}", source.display()))?;
    let mut header = [0u8; 92];
    input.seek(SeekFrom::Start(sector_size))?;
    input
        .read_exact(&mut header)
        .context("failed to read GPT header")?;
    anyhow::ensure!(
        &header[..8] == b"EFI PART",
        "disk has no primary GPT header"
    );
    let header_size = u32::from_le_bytes(header[12..16].try_into().unwrap());
    anyhow::ensure!(
        (92..=sector_size as u32).contains(&header_size),
        "invalid GPT header size"
    );
    let entries_lba = u64::from_le_bytes(header[72..80].try_into().unwrap());
    let entry_count = u32::from_le_bytes(header[80..84].try_into().unwrap());
    let entry_size = u32::from_le_bytes(header[84..88].try_into().unwrap());
    anyhow::ensure!(
        index <= entry_count,
        "GPT partition index exceeds table size"
    );
    anyhow::ensure!(
        (128..=4096).contains(&entry_size) && entry_size % 8 == 0,
        "invalid GPT entry size"
    );
    let entry_offset = entries_lba
        .checked_mul(sector_size)
        .and_then(|offset| offset.checked_add(u64::from(index - 1) * u64::from(entry_size)))
        .context("GPT entry offset overflow")?;
    let mut entry = vec![0u8; entry_size as usize];
    input.seek(SeekFrom::Start(entry_offset))?;
    input
        .read_exact(&mut entry)
        .context("failed to read GPT partition entry")?;
    anyhow::ensure!(
        entry[..16].iter().any(|byte| *byte != 0),
        "GPT partition is unused"
    );
    let first_lba = u64::from_le_bytes(entry[32..40].try_into().unwrap());
    let last_lba = u64::from_le_bytes(entry[40..48].try_into().unwrap());
    anyhow::ensure!(last_lba >= first_lba, "GPT partition has inverted bounds");
    let offset = first_lba
        .checked_mul(sector_size)
        .context("GPT partition offset overflow")?;
    let length = last_lba
        .checked_sub(first_lba)
        .and_then(|sectors| sectors.checked_add(1))
        .and_then(|sectors| sectors.checked_mul(sector_size))
        .context("GPT partition length overflow")?;
    anyhow::ensure!(
        offset
            .checked_add(length)
            .is_some_and(|end| end <= source_len),
        "GPT partition exceeds disk image"
    );
    Ok((offset, length))
}

fn extract_gpt_partition(
    source: &Path,
    dest: &Path,
    index: &str,
    sector_size: Option<&str>,
) -> anyhow::Result<()> {
    anyhow::ensure!(
        source != dest,
        "GPT source and partition output must differ"
    );
    let (offset, length) = gpt_partition_range(source, index, sector_size)?;
    let source_id = source_file_identity(source)?;
    let source_stamp = PathBuf::from(format!("{}.source", dest.display()));
    if dest.is_file()
        && fs::metadata(dest).map(|meta| meta.len()).unwrap_or(0) == length
        && fs::read_to_string(&source_stamp).unwrap_or_default() == source_id
    {
        return Ok(());
    }
    let mut input = fs::File::open(source)?;

    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("failed to create {}", parent.display()))?;
    }
    let tmp = dest.with_extension("tmp");
    let _ = fs::remove_file(&tmp);
    let mut output = OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&tmp)
        .with_context(|| format!("failed to create {}", tmp.display()))?;
    input.seek(SeekFrom::Start(offset))?;
    let mut remaining = length;
    let mut buffer = vec![0u8; 1024 * 1024];
    while remaining > 0 {
        let count = usize::try_from(remaining.min(buffer.len() as u64)).unwrap();
        input.read_exact(&mut buffer[..count])?;
        if buffer[..count].iter().all(|byte| *byte == 0) {
            output.seek(SeekFrom::Current(count as i64))?;
        } else {
            output.write_all(&buffer[..count])?;
        }
        remaining -= count as u64;
    }
    output.set_len(length)?;
    output.sync_all()?;
    fs::rename(&tmp, dest)
        .with_context(|| format!("failed to move {} to {}", tmp.display(), dest.display()))?;
    write_output(&source_stamp, source_id.as_bytes())?;
    println!(
        "[fetch-guest] Extracted GPT partition {} ({} bytes) -> {}",
        index,
        length,
        dest.display()
    );
    Ok(())
}

/// Install configuration in a private disk copy, never the acquired base image.
/// Guest executables are not run on the host and host keys are not generated here.
fn install_gpt_ext4_file(
    source: &Path,
    dest: &Path,
    index: &str,
    sector_size: Option<&str>,
    filesystem_path: &str,
    content: &[u8],
) -> anyhow::Result<()> {
    anyhow::ensure!(source != dest, "disk source and output must differ");
    let path = Path::new(filesystem_path);
    anyhow::ensure!(
        path.is_absolute()
            && path.file_name().is_some()
            && path
                .components()
                .all(|c| matches!(c, Component::RootDir | Component::Normal(_)))
            && filesystem_path
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"/_+.-".contains(&b)),
        "installed ext4 path must be confined and absolute"
    );
    anyhow::ensure!(
        !content.is_empty() && content.len() <= 65536,
        "installed configuration must contain 1..65536 bytes"
    );
    let (offset, length) = gpt_partition_range(source, index, sector_size)?;
    let source_id = source_file_identity(source)?;
    let recipe = serde_json::to_vec(&(
        "gpt-ext4-file-v1",
        &source_id,
        index,
        sector_size.unwrap_or("512"),
        filesystem_path,
        content,
    ))?;
    let identity = format!("{:x}", Sha512::digest(&recipe));
    let stamp = PathBuf::from(format!("{}.source", dest.display()));
    if dest.is_file()
        && fs::metadata(dest)?.len() == fs::metadata(source)?.len()
        && fs::read_to_string(&stamp).unwrap_or_default() == identity
    {
        return Ok(());
    }
    anyhow::ensure!(!dest.exists(),
        "configured disk already exists with a different source/recipe; choose a new output path to preserve guest data");
    let parent = dest.parent().context("disk output has no parent")?;
    fs::create_dir_all(parent)?;
    let work = tempfile::Builder::new()
        .prefix("guest-config-")
        .tempdir_in(parent)?;
    let partition = work.path().join("partition.ext4");
    extract_gpt_partition(source, &partition, index, sector_size)?;
    let partition = fs::canonicalize(partition)?;
    fs::write(work.path().join("content"), content)?;
    let debugfs = find_tool(&[
        "debugfs",
        "/opt/homebrew/opt/e2fsprogs/sbin/debugfs",
        "/usr/local/opt/e2fsprogs/sbin/debugfs",
        "/usr/sbin/debugfs",
        "/usr/bin/debugfs",
    ])?;
    let mut commands = String::new();
    let mut directory = String::new();
    for component in path
        .parent()
        .context("configuration has no parent")?
        .components()
    {
        if let Component::Normal(name) = component {
            directory.push('/');
            directory.push_str(name.to_str().context("non-UTF8 configuration path")?);
            commands.push_str(&format!("mkdir {directory}\n"));
        }
    }
    // mkdir of an existing directory and rm of an absent file are harmless.
    // debugfs can exit successfully after a command error, so verify data below.
    commands.push_str(&format!(
        "rm {filesystem_path}\nwrite content {filesystem_path}\nset_inode_field {filesystem_path} mode 0100644\nset_inode_field {filesystem_path} uid 0\nset_inode_field {filesystem_path} gid 0\ndump {filesystem_path} readback\n"
    ));
    fs::write(work.path().join("commands"), commands)?;
    let output = std::process::Command::new(debugfs)
        .args(["-w", "-f", "commands"])
        .arg(&partition)
        .current_dir(work.path())
        .output()?;
    anyhow::ensure!(
        output.status.success(),
        "debugfs configuration install failed"
    );
    anyhow::ensure!(
        fs::read(work.path().join("readback"))? == content,
        "debugfs configuration readback differs"
    );
    anyhow::ensure!(
        fs::metadata(&partition)?.len() == length,
        "configuration installation changed partition length"
    );
    let disk = work.path().join("disk.raw");
    fs::copy(source, &disk)?;
    let mut output = OpenOptions::new().write(true).open(&disk)?;
    output.seek(SeekFrom::Start(offset))?;
    let copied = std::io::copy(&mut fs::File::open(partition)?.take(length), &mut output)?;
    anyhow::ensure!(copied == length, "short configured partition copy");
    output.sync_all()?;
    anyhow::ensure!(
        source_file_identity(source)? == source_id,
        "base disk changed during configuration install"
    );
    // Atomic no-clobber publication on the same filesystem. A competing
    // preparation must not replace a writable disk created since our check.
    fs::hard_link(disk, dest)?;
    write_output(&stamp, identity.as_bytes())?;
    Ok(())
}

fn extract_ext4_file(source: &Path, dest: &Path, filesystem_path: &str) -> anyhow::Result<()> {
    anyhow::ensure!(source != dest, "ext4 source and output must differ");
    let path = Path::new(filesystem_path);
    anyhow::ensure!(
        path.is_absolute()
            && path.components().all(|component| {
                matches!(component, Component::RootDir | Component::Normal(_))
            })
            && filesystem_path
                .bytes()
                .all(|byte| { byte.is_ascii_alphanumeric() || b"/_+.-".contains(&byte) }),
        "ext4 path must be a confined absolute path without shell syntax"
    );
    let source_id = source_file_identity(source)?;
    let source_stamp = PathBuf::from(format!("{}.source", dest.display()));
    if dest.is_file()
        && fs::metadata(dest).map(|meta| meta.len()).unwrap_or(0) > 0
        && fs::read_to_string(&source_stamp).unwrap_or_default() == source_id
    {
        println!(
            "[fetch-guest] ext4 file already extracted: {}",
            dest.display()
        );
        return Ok(());
    }
    let parent = dest
        .parent()
        .context("ext4 output has no parent directory")?;
    fs::create_dir_all(parent).with_context(|| format!("failed to create {}", parent.display()))?;
    let tmp = dest.with_extension("tmp");
    let _ = fs::remove_file(&tmp);
    let tmp_name = tmp
        .file_name()
        .and_then(|name| name.to_str())
        .context("ext4 output name is not UTF-8")?;
    anyhow::ensure!(
        tmp_name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || b"_+.-".contains(&byte)),
        "ext4 output name contains unsupported characters"
    );
    let debugfs = find_tool(&[
        "debugfs",
        "/opt/homebrew/opt/e2fsprogs/sbin/debugfs",
        "/usr/local/opt/e2fsprogs/sbin/debugfs",
        "/usr/sbin/debugfs",
        "/usr/bin/debugfs",
    ])?;
    let status = std::process::Command::new(&debugfs)
        .args(["-R", &format!("dump -p {filesystem_path} {tmp_name}")])
        .arg(source)
        .current_dir(parent)
        .status()
        .with_context(|| format!("failed to run {}", debugfs.display()))?;
    anyhow::ensure!(status.success(), "debugfs extraction failed with {status}");
    anyhow::ensure!(
        fs::metadata(&tmp).map(|meta| meta.len()).unwrap_or(0) > 0,
        "debugfs produced an empty output"
    );
    fs::rename(&tmp, dest)
        .with_context(|| format!("failed to move {} to {}", tmp.display(), dest.display()))?;
    write_output(&source_stamp, source_id.as_bytes())?;
    println!(
        "[fetch-guest] Extracted ext4 file {} -> {}",
        filesystem_path,
        dest.display()
    );
    Ok(())
}

fn convert_qcow2_to_raw(source: &Path, dest: &Path) -> anyhow::Result<()> {
    anyhow::ensure!(source != dest, "qcow2 source and raw output must differ");
    anyhow::ensure!(
        source.is_file() && fs::metadata(source).map(|meta| meta.len()).unwrap_or(0) > 0,
        "qcow2 source is missing or empty: {}",
        source.display()
    );
    let source_id = source_file_identity(source)?;
    let source_stamp = PathBuf::from(format!("{}.source", dest.display()));
    if dest.is_file()
        && fs::metadata(dest).map(|meta| meta.len()).unwrap_or(0) > 0
        && fs::read_to_string(&source_stamp).unwrap_or_default() == source_id
    {
        println!(
            "[fetch-guest] raw image already converted: {}",
            dest.display()
        );
        return Ok(());
    }
    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("failed to create {}", parent.display()))?;
    }
    let tmp = dest.with_extension("tmp");
    let _ = fs::remove_file(&tmp);
    let qemu_img = find_tool(&[
        "qemu-img",
        "/opt/homebrew/bin/qemu-img",
        "/usr/local/bin/qemu-img",
        "/usr/bin/qemu-img",
    ])?;
    let status = std::process::Command::new(&qemu_img)
        .args(["convert", "-f", "qcow2", "-O", "raw", "-S", "4k"])
        .arg(source)
        .arg(&tmp)
        .status()
        .with_context(|| format!("failed to run {}", qemu_img.display()))?;
    anyhow::ensure!(status.success(), "qemu-img convert failed with {status}");
    anyhow::ensure!(
        fs::metadata(&tmp).map(|meta| meta.len()).unwrap_or(0) > 0,
        "qemu-img produced an empty raw image"
    );
    fs::rename(&tmp, dest)
        .with_context(|| format!("failed to move {} to {}", tmp.display(), dest.display()))?;
    write_output(&source_stamp, source_id.as_bytes())?;
    println!("[fetch-guest] Converted qcow2 to raw: {}", dest.display());
    Ok(())
}

fn verify_sha512(path: &Path, expected: &str) -> anyhow::Result<()> {
    let mut input = fs::File::open(path)
        .with_context(|| format!("failed to open {} for SHA-512", path.display()))?;
    let mut digest = Sha512::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let bytes = input
            .read(&mut buffer)
            .with_context(|| format!("failed to hash {}", path.display()))?;
        if bytes == 0 {
            break;
        }
        digest.update(&buffer[..bytes]);
    }
    let actual = format!("{:x}", digest.finalize());
    anyhow::ensure!(
        actual.eq_ignore_ascii_case(expected),
        "SHA-512 mismatch for {}: expected {}, got {}",
        path.display(),
        expected,
        actual
    );
    println!("[fetch-guest] SHA-512 verified: {}", path.display());
    Ok(())
}

fn verify_sha256(path: &Path, expected: &str) -> anyhow::Result<()> {
    let mut input = fs::File::open(path)
        .with_context(|| format!("failed to open {} for SHA-256", path.display()))?;
    let mut digest = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let bytes = input
            .read(&mut buffer)
            .with_context(|| format!("failed to hash {}", path.display()))?;
        if bytes == 0 {
            break;
        }
        digest.update(&buffer[..bytes]);
    }
    let actual = format!("{:x}", digest.finalize());
    anyhow::ensure!(
        actual.eq_ignore_ascii_case(expected),
        "SHA-256 mismatch for {}: expected {}, got {}",
        path.display(),
        expected,
        actual
    );
    println!("[fetch-guest] SHA-256 verified: {}", path.display());
    Ok(())
}

fn build_static_linux_elf(step: &RecipeStep, root: &Path, output_dir: &Path) -> anyhow::Result<()> {
    let target = match recipe_arg(step, "architecture")? {
        "x86_64" => "x86_64-unknown-linux-gnu",
        "aarch64" => "aarch64-unknown-linux-gnu",
        other => anyhow::bail!("unsupported native helper architecture {other:?}"),
    };
    let source = recipe_path(root, recipe_arg(step, "source")?)?;
    anyhow::ensure!(
        source.extension().is_some_and(|ext| ext == "c"),
        "native helper source must be C"
    );
    let output = recipe_path(output_dir, recipe_arg(step, "output")?)?;
    if let Some(parent) = output.parent() {
        fs::create_dir_all(parent)?;
    }
    let temp = output.with_extension("elf.tmp");
    // This freestanding build uses Clang resource headers and LLD, not a
    // discovered host GCC installation or its target runtime libraries.
    let empty_toolchain = tempfile::tempdir()?;
    let tool_path = std::env::var_os("AGENTOS_HOST_TOOL_PATH")
        .or_else(|| std::env::var_os("PATH"))
        .context("native helper compiler search path is unavailable")?;
    let status = std::process::Command::new("clang")
        .env("PATH", &tool_path)
        .arg(format!(
            "--gcc-toolchain={}",
            empty_toolchain.path().display()
        ))
        .args([
            "-target",
            target,
            "-ffreestanding",
            "-fno-builtin",
            "-fno-stack-protector",
            "-fno-pie",
            "-nostdlib",
            "-static",
            "-fuse-ld=lld",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wl,--build-id=none",
            "-Wl,-e,_start",
        ])
        .arg(&source)
        .arg("-o")
        .arg(&temp)
        .status()
        .context("compile native Linux helper")?;
    anyhow::ensure!(status.success(), "native Linux helper compilation failed");
    let objcopy = find_tool(&[
        "llvm-objcopy",
        "/opt/homebrew/opt/llvm/bin/llvm-objcopy",
        "/opt/homebrew/opt/llvm@22/bin/llvm-objcopy",
        "/opt/homebrew/opt/llvm@21/bin/llvm-objcopy",
        "/usr/local/opt/llvm/bin/llvm-objcopy",
        "/usr/bin/llvm-objcopy",
    ])?;
    let status = std::process::Command::new(&objcopy)
        .args(["--strip-all", "--remove-section=.comment"])
        .arg(&temp)
        .status()
        .with_context(|| format!("failed to run {}", objcopy.display()))?;
    anyhow::ensure!(status.success(), "native Linux helper normalization failed");
    fs::rename(temp, output)?;
    Ok(())
}

fn initramfs_payload(step: &RecipeStep, output_dir: &Path) -> anyhow::Result<Vec<u8>> {
    match (step.args.get("content"), step.args.get("content_file")) {
        (Some(content), None) => {
            anyhow::ensure!(
                !step.args.contains_key("content_sha256"),
                "content_sha256 requires content_file"
            );
            Ok(content.as_bytes().to_vec())
        }
        (None, Some(path)) => {
            let expected = recipe_arg(step, "content_sha256")?;
            anyhow::ensure!(
                expected.len() == 64 && expected.bytes().all(|b| b.is_ascii_hexdigit()),
                "content_sha256 must contain 64 hexadecimal digits"
            );
            let path = recipe_path(output_dir, path)?;
            let file = fs::File::open(&path)
                .with_context(|| format!("open initramfs payload {}", path.display()))?;
            anyhow::ensure!(
                file.metadata()?.is_file(),
                "initramfs payload must be a regular file"
            );
            const LIMIT: u64 = 16 * 1024 * 1024;
            let mut bytes = Vec::new();
            file.take(LIMIT + 1).read_to_end(&mut bytes)?;
            anyhow::ensure!(
                bytes.len() as u64 <= LIMIT,
                "initramfs payload exceeds 16 MiB"
            );
            let actual = format!("{:x}", Sha256::digest(&bytes));
            anyhow::ensure!(
                actual.eq_ignore_ascii_case(expected),
                "initramfs payload SHA-256 mismatch for {}",
                path.display()
            );
            Ok(bytes)
        }
        _ => anyhow::bail!("initramfs requires exactly one of content or content_file"),
    }
}

fn append_initramfs_file(
    source: &Path,
    dest: &Path,
    archive_path: &str,
    mode: &str,
    content: &[u8],
    compression: Option<&str>,
) -> anyhow::Result<()> {
    anyhow::ensure!(source != dest, "initramfs source and output must differ");
    anyhow::ensure!(
        source.is_file() && fs::metadata(source).map(|meta| meta.len()).unwrap_or(0) > 0,
        "initramfs source is missing or empty: {}",
        source.display()
    );
    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("failed to create {}", parent.display()))?;
    }
    let tmp = dest.with_extension("tmp");
    let _ = fs::remove_file(&tmp);
    fs::copy(source, &tmp).with_context(|| {
        format!(
            "failed to copy base initramfs {} to {}",
            source.display(),
            tmp.display()
        )
    })?;

    let encoded = encode_initramfs_file(archive_path, mode, content, compression)?;

    let mut out = OpenOptions::new()
        .append(true)
        .open(&tmp)
        .with_context(|| format!("failed to append to {}", tmp.display()))?;
    if compression.unwrap_or("none") == "none" {
        let len = out.metadata()?.len();
        let padding = (4 - len % 4) % 4;
        if padding != 0 {
            out.write_all(&[0u8; 3][..padding as usize])?;
        }
    }
    out.write_all(&encoded)?;
    out.flush()?;
    drop(out);
    fs::rename(&tmp, dest)
        .with_context(|| format!("failed to move {} to {}", tmp.display(), dest.display()))?;
    println!(
        "[fetch-guest] Appended initramfs file {} -> {}",
        archive_path,
        dest.display()
    );
    Ok(())
}

fn parse_newc_hex(bytes: &[u8], field: &str) -> anyhow::Result<usize> {
    let text = std::str::from_utf8(bytes).with_context(|| format!("newc {field} is not ASCII"))?;
    usize::from_str_radix(text, 16).with_context(|| format!("newc {field} is not hexadecimal"))
}

fn filter_initramfs_modules(source: &Path, dest: &Path, modules: &str) -> anyhow::Result<()> {
    anyhow::ensure!(source != dest, "initramfs source and output must differ");
    let requested: Vec<&str> = modules.split(',').collect();
    anyhow::ensure!(
        !requested.is_empty()
            && requested.len() <= 16
            && requested.iter().all(|name| {
                !name.is_empty()
                    && name.len() <= 64
                    && name
                        .bytes()
                        .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
            }),
        "initramfs module selection must contain 1..16 bounded module names"
    );
    let mut unique = requested.clone();
    unique.sort_unstable();
    unique.dedup();
    anyhow::ensure!(
        unique.len() == requested.len(),
        "duplicate initramfs module"
    );

    let source_identity = source_file_identity(source)?;
    let recipe_identity = format!("{source_identity}\nmodules={modules}\n");
    let source_stamp = PathBuf::from(format!("{}.source", dest.display()));
    if dest.is_file()
        && fs::metadata(dest)
            .map(|metadata| metadata.len())
            .unwrap_or(0)
            > 0
        && fs::read_to_string(&source_stamp).unwrap_or_default() == recipe_identity
    {
        println!(
            "[fetch-guest] Filtered initramfs already staged: {}",
            dest.display()
        );
        return Ok(());
    }

    let input = fs::read(source).with_context(|| format!("read {}", source.display()))?;
    anyhow::ensure!(
        input.len() <= 512 * 1024 * 1024,
        "source initramfs exceeds 512 MiB"
    );
    let mut offset = 0usize;
    let mut selected: Vec<(String, u32, Vec<u8>)> = Vec::new();
    let mut found = std::collections::BTreeSet::new();
    loop {
        anyhow::ensure!(
            offset
                .checked_add(110)
                .is_some_and(|end| end <= input.len())
                && &input[offset..offset + 6] == b"070701",
            "source initramfs does not start with one bounded newc archive"
        );
        let header = &input[offset..offset + 110];
        let mode = parse_newc_hex(&header[14..22], "mode")? as u32;
        let file_size = parse_newc_hex(&header[54..62], "file size")?;
        let name_size = parse_newc_hex(&header[94..102], "name size")?;
        anyhow::ensure!(name_size > 0 && name_size <= 4096, "invalid newc name size");
        let name_start = offset + 110;
        let name_end = name_start
            .checked_add(name_size)
            .context("newc name offset overflow")?;
        anyhow::ensure!(name_end <= input.len(), "truncated newc name");
        anyhow::ensure!(input[name_end - 1] == 0, "newc name is not terminated");
        let name = std::str::from_utf8(&input[name_start..name_end - 1])
            .context("newc name is not UTF-8")?;
        let data_start = (name_end + 3) & !3;
        let data_end = data_start
            .checked_add(file_size)
            .context("newc data offset overflow")?;
        anyhow::ensure!(data_end <= input.len(), "truncated newc data");
        offset = (data_end + 3) & !3;
        if name == "TRAILER!!!" {
            break;
        }
        if name.starts_with("usr/lib/modules/") {
            let filename = Path::new(name)
                .file_name()
                .and_then(|name| name.to_str())
                .unwrap_or_default();
            if let Some(module) = requested.iter().find(|module| {
                filename == format!("{module}.ko") || filename == format!("{module}.ko.zst")
            }) {
                anyhow::ensure!(
                    found.insert((*module).to_owned()),
                    "initramfs module {module:?} is ambiguous"
                );
                selected.push((
                    name.to_owned(),
                    mode & 0o777,
                    input[data_start..data_end].to_vec(),
                ));
            }
        }
    }
    for module in &requested {
        anyhow::ensure!(
            found.contains(*module),
            "initramfs module {module:?} is missing"
        );
    }
    let xz_magic = [0xfdu8, b'7', b'z', b'X', b'Z', 0];
    let xz_offset = input[offset..]
        .windows(xz_magic.len())
        .position(|window| window == xz_magic)
        .map(|position| offset + position)
        .context("source initramfs has no trailing XZ archive")?;

    selected.sort_by(|left, right| left.0.cmp(&right.0));
    let mut out = Vec::new();
    let mut ino = 1u32;
    let mut directories = std::collections::BTreeSet::new();
    for (name, _, _) in &selected {
        let mut parent = Path::new(name).parent();
        while let Some(path) = parent.filter(|path| !path.as_os_str().is_empty()) {
            directories.insert(path.to_string_lossy().into_owned());
            parent = path.parent();
        }
    }
    for directory in directories {
        append_newc_dir(&mut out, &directory, ino)?;
        ino += 1;
    }
    for (name, mode, data) in selected {
        append_newc_file(&mut out, &name, ino, mode, &data)?;
        ino += 1;
    }
    append_newc_trailer(&mut out, ino)?;
    out.extend_from_slice(&input[xz_offset..]);
    anyhow::ensure!(
        out.len() <= 64 * 1024 * 1024,
        "filtered initramfs exceeds the 64 MiB x86 embedded boot bound"
    );
    write_output(dest, &out)?;
    write_output(&source_stamp, recipe_identity.as_bytes())?;
    println!(
        "[fetch-guest] Retained modules {modules} in {} ({} bytes)",
        dest.display(),
        out.len()
    );
    Ok(())
}

fn encode_initramfs_file(
    archive_path: &str,
    mode: &str,
    content: &[u8],
    compression: Option<&str>,
) -> anyhow::Result<Vec<u8>> {
    let path = Path::new(archive_path);
    anyhow::ensure!(
        !archive_path.is_empty()
            && !path.is_absolute()
            && path
                .components()
                .all(|component| matches!(component, Component::Normal(_))),
        "initramfs archive path must be confined and relative: {archive_path:?}"
    );
    let mode = u32::from_str_radix(mode, 8).context("initramfs mode must be octal")?;
    anyhow::ensure!(mode <= 0o777, "initramfs mode exceeds 0777");

    let mut overlay = Vec::new();
    let mut ino = 1u32;
    let mut parent = PathBuf::new();
    if let Some(components) = path.parent() {
        for component in components.components() {
            let Component::Normal(name) = component else {
                unreachable!("archive path was validated above")
            };
            parent.push(name);
            append_newc_dir(&mut overlay, &parent.to_string_lossy(), ino)?;
            ino += 1;
        }
    }
    append_newc_file(&mut overlay, archive_path, ino, mode, content)?;
    ino += 1;
    append_newc_trailer(&mut overlay, ino)?;

    let encoded = match compression.unwrap_or("none") {
        "none" => overlay,
        "zstd" => zstd::stream::encode_all(&overlay[..], 3)
            .context("failed to compress initramfs overlay as zstd")?,
        other => anyhow::bail!("unsupported initramfs overlay compression {other:?}"),
    };
    Ok(encoded)
}

fn build_initramfs_file(
    dest: &Path,
    archive_path: &str,
    mode: &str,
    content: &[u8],
    compression: Option<&str>,
) -> anyhow::Result<()> {
    let encoded = encode_initramfs_file(archive_path, mode, content, compression)?;
    write_output(dest, &encoded)?;
    println!(
        "[fetch-guest] Built initramfs file {} -> {}",
        archive_path,
        dest.display()
    );
    Ok(())
}

fn build_linux_probe_initramfs(initrd_dest: &Path) -> anyhow::Result<()> {
    if linux_probe_initramfs_ready(initrd_dest)? {
        println!(
            "[fetch-guest] Linux probe initramfs already staged: {}",
            initrd_dest.display()
        );
        return Ok(());
    }

    let tmp_root = build_tmp_dir()?;
    let tmp_dir = tempfile::Builder::new()
        .prefix("agentos-linux-probe-initrd-")
        .tempdir_in(&tmp_root)
        .context("failed to create Linux probe initrd tempdir under _build/tmp")?;
    let init = build_linux_e2e_init(tmp_dir.path())?;
    let out = create_linux_probe_initramfs(&init)?;
    write_output(initrd_dest, &out)?;
    println!(
        "[fetch-guest] Built deterministic Linux probe initramfs -> {}",
        initrd_dest.display()
    );
    Ok(())
}

fn linux_probe_initramfs_ready(initrd: &Path) -> anyhow::Result<bool> {
    if !initrd.exists() || fs::metadata(initrd).map(|m| m.len()).unwrap_or(0) == 0 {
        return Ok(false);
    }
    let entries = archive_entries(initrd).unwrap_or_default();
    Ok(entries
        .iter()
        .any(|entry| entry == "init" || entry == "./init")
        && entries
            .iter()
            .any(|entry| entry == "agentos-init-v5" || entry == "./agentos-init-v5"))
}

fn build_linux_e2e_init(work_dir: &Path) -> anyhow::Result<Vec<u8>> {
    build_static_init(work_dir, LINUX_E2E_INIT_ASM, "aarch64-linux-gnu")
}

pub fn build_x86_initramfs() -> anyhow::Result<()> {
    for (mode, suffix) in [(0, ""), (1, "-write"), (2, "-verify")] {
        let root = build_tmp_dir()?;
        let tmp = tempfile::Builder::new()
            .prefix("x86-init-")
            .tempdir_in(root)?;
        let init = build_static_init(
            tmp.path(),
            &format!(
                ".set STORAGE_MODE, {mode}\n{}",
                include_str!("../../tests/platform/x86_linux_init.S")
            ),
            "x86_64-linux-gnu",
        )?;
        let mut archive = Vec::new();
        append_newc_dir(&mut archive, ".", 1)?;
        append_newc_file(&mut archive, "init", 2, 0o755, &init)?;
        append_newc_trailer(&mut archive, 3)?;
        write_output(
            &repo_root()?.join(format!("_build/x86-userspace/initrd{suffix}.bin")),
            &archive,
        )?;
        println!("[x86-userspace] Built _build/x86-userspace/initrd{suffix}.bin");
    }
    Ok(())
}

fn build_static_init(work_dir: &Path, source: &str, target: &str) -> anyhow::Result<Vec<u8>> {
    let init_s = work_dir.join("agentos-linux-e2e-init.S");
    let init_elf = work_dir.join("init");
    let normalized_elf = work_dir.join("init.normalized");
    fs::write(&init_s, source).with_context(|| format!("failed to write {}", init_s.display()))?;

    let clang = find_tool(&[
        "clang",
        "/opt/homebrew/opt/llvm/bin/clang",
        "/opt/homebrew/opt/llvm@22/bin/clang",
        "/opt/homebrew/opt/llvm@21/bin/clang",
        "/usr/bin/clang",
    ])?;
    let status = std::process::Command::new(&clang)
        .args([
            "-target",
            target,
            "-nostdlib",
            "-static",
            "-fuse-ld=lld",
            "-Wl,-e,_start",
            "-Wl,--build-id=none",
        ])
        .arg(&init_s)
        .arg("-o")
        .arg(&init_elf)
        .status()
        .with_context(|| format!("failed to run {}", clang.display()))?;
    anyhow::ensure!(
        status.success(),
        "{} failed building E2E init",
        clang.display()
    );

    // Clang and LLD identify their host toolchain in non-loadable ELF sections.
    // Normalize those sections so the pinned initramfs and derived DTB hashes
    // are identical on Linux and macOS without weakening artifact verification.
    let objcopy = find_tool(&[
        "llvm-objcopy",
        "/opt/homebrew/opt/llvm/bin/llvm-objcopy",
        "/opt/homebrew/opt/llvm@22/bin/llvm-objcopy",
        "/opt/homebrew/opt/llvm@21/bin/llvm-objcopy",
        "/usr/local/opt/llvm/bin/llvm-objcopy",
        "/usr/bin/llvm-objcopy",
    ])?;
    let status = std::process::Command::new(&objcopy)
        .args(["--strip-all", "--remove-section=.comment"])
        .arg(&init_elf)
        .arg(&normalized_elf)
        .status()
        .with_context(|| format!("failed to run {}", objcopy.display()))?;
    anyhow::ensure!(
        status.success(),
        "{} failed normalizing E2E init",
        objcopy.display()
    );

    let init = fs::read(&normalized_elf)
        .with_context(|| format!("failed to read {}", normalized_elf.display()))?;
    anyhow::ensure!(
        init.starts_with(b"\x7fELF"),
        "built E2E init is not an ELF binary"
    );
    Ok(init)
}

fn create_linux_probe_initramfs(init_elf: &[u8]) -> anyhow::Result<Vec<u8>> {
    let mut out = Vec::new();
    let mut ino = 1u32;
    append_newc_dir(&mut out, ".", ino)?;
    ino += 1;
    append_newc_dir(&mut out, "dev", ino)?;
    ino += 1;
    append_newc_chr(&mut out, "dev/console", ino, 0o600, 5, 1)?;
    ino += 1;
    append_newc_chr(&mut out, "dev/null", ino, 0o666, 1, 3)?;
    ino += 1;
    append_newc_file(&mut out, "init", ino, 0o755, init_elf)?;
    ino += 1;
    append_newc_file(
        &mut out,
        "agentos-init-v5",
        ino,
        0o444,
        b"console-open\nvirtio-net-frame\n",
    )?;
    ino += 1;
    append_newc_trailer(&mut out, ino)?;
    Ok(out)
}

fn append_newc_dir(out: &mut Vec<u8>, name: &str, ino: u32) -> anyhow::Result<()> {
    append_newc_entry(out, name, ino, 0o040755, 2, 0, 0, 0, 0, &[])
}

fn append_newc_chr(
    out: &mut Vec<u8>,
    name: &str,
    ino: u32,
    perms: u32,
    major: u32,
    minor: u32,
) -> anyhow::Result<()> {
    append_newc_entry(out, name, ino, 0o020000 | perms, 1, 0, 0, major, minor, &[])
}

fn append_newc_file(
    out: &mut Vec<u8>,
    name: &str,
    ino: u32,
    perms: u32,
    data: &[u8],
) -> anyhow::Result<()> {
    append_newc_entry(out, name, ino, 0o100000 | perms, 1, 0, 0, 0, 0, data)
}

fn append_newc_trailer(out: &mut Vec<u8>, ino: u32) -> anyhow::Result<()> {
    append_newc_entry(out, "TRAILER!!!", ino, 0, 1, 0, 0, 0, 0, &[])
}

fn append_newc_entry(
    out: &mut Vec<u8>,
    name: &str,
    ino: u32,
    mode: u32,
    nlink: u32,
    uid: u32,
    gid: u32,
    rdevmajor: u32,
    rdevminor: u32,
    data: &[u8],
) -> anyhow::Result<()> {
    let namesize = name.len() + 1;
    let filesize = u32::try_from(data.len()).context("initramfs entry too large")?;
    let header = format!(
        "070701{ino:08x}{mode:08x}{uid:08x}{gid:08x}{nlink:08x}{mtime:08x}{filesize:08x}{devmajor:08x}{devminor:08x}{rdevmajor:08x}{rdevminor:08x}{namesize:08x}{check:08x}",
        mtime = 0u32,
        devmajor = 0u32,
        devminor = 0u32,
        check = 0u32
    );
    anyhow::ensure!(header.len() == 110, "newc header length mismatch");
    out.extend_from_slice(header.as_bytes());
    out.extend_from_slice(name.as_bytes());
    out.push(0);
    pad_newc(out);
    out.extend_from_slice(data);
    pad_newc(out);
    Ok(())
}

fn pad_newc(out: &mut Vec<u8>) {
    while out.len() % 4 != 0 {
        out.push(0);
    }
}

const LINUX_E2E_INIT_ASM: &str = r#"
.section .text
.global _start
_start:
    bl   net_probe

    mov  x0, #-100
    adrp x1, dev_console
    add  x1, x1, :lo12:dev_console
    mov  x2, #2
    mov  x3, #0
    mov  x8, #56
    svc  #0
    cmp  x0, #0
    b.ge 0f
    mov  x19, #0
    b 2f
0:
    mov  x19, x0
2:
    adrp x1, banner
    add  x1, x1, :lo12:banner
    mov  x0, #1
    mov  x2, #banner_len
    mov  x8, #64
    svc  #0

1:
    mov  x0, x19
    adrp x1, inbuf
    add  x1, x1, :lo12:inbuf
    mov  x2, #1
    mov  x8, #63
    svc  #0
    cmp  x0, #1
    b.eq 3f
    mov  x8, #124
    svc  #0
    b 1b

3:
    mov  x0, #1
    adrp x1, inbuf
    add  x1, x1, :lo12:inbuf
    mov  x2, #1
    mov  x8, #64
    svc  #0

    ldrb w3, [x1]
    cmp  w3, #33                 /* '!' requests the bounded stress stream. */
    b.eq stress_emit
    cmp  w3, #10
    b.ne 1b

    mov  x0, #1
    adrp x1, prompt
    add  x1, x1, :lo12:prompt
    mov  x2, #prompt_len
    mov  x8, #64
    svc  #0
    b 1b

/* Emit more bytes than all serial queues combined. Printable payload avoids
 * tty newline translation; each byte depends on its stream position. */
stress_emit:
    adrp x6, stress_payload
    add  x6, x6, :lo12:stress_payload
    mov  x5, #0
    mov  x4, #0x40000
stress_fill:
    eor  w7, w5, w5, lsr #8
    eor  w7, w7, w5, lsr #16
    and  w7, w7, #15
    add  w7, w7, #65
    strb w7, [x6, x5]
    add  x5, x5, #1
    cmp  x5, x4
    b.lo stress_fill
    adrp x21, stress_begin
    add  x21, x21, :lo12:stress_begin
    mov  x20, #stress_begin_len
    bl stress_write_all
    adrp x21, stress_payload
    add  x21, x21, :lo12:stress_payload
    mov  x20, #0x40000
    bl stress_write_all
    adrp x21, stress_end
    add  x21, x21, :lo12:stress_end
    mov  x20, #stress_end_len
    bl stress_write_all
    b 1b
stress_write_all:
    mov  x0, #1
    mov  x1, x21
    mov  x2, x20
    mov  x8, #64
    svc  #0
    cmp  x0, #0
    b.le stress_write_all
    add  x21, x21, x0
    sub  x20, x20, x0
    cbnz x20, stress_write_all
    ret

/* Bring eth0 up and emit one Ethernet frame through the guest virtio NIC. */
net_probe:
    mov  x0, #2                  /* AF_INET */
    mov  x1, #2                  /* SOCK_DGRAM */
    mov  x2, #0
    mov  x8, #198                /* socket */
    svc  #0
    cmp  x0, #0
    b.lt 8f
    mov  x20, x0
    adrp x2, ifreq
    add  x2, x2, :lo12:ifreq
    mov  x1, #0x8914            /* SIOCSIFFLAGS */
    mov  x0, x20
    mov  x8, #29                 /* ioctl */
    svc  #0
    mov  x0, x20
    mov  x8, #57                 /* close */
    svc  #0

    mov  x0, #17                 /* AF_PACKET */
    mov  x1, #3                  /* SOCK_RAW */
    mov  x2, #0xb588            /* htons(ETH_P_802_EX1 / 0x88b5) */
    mov  x8, #198                /* socket */
    svc  #0
    cmp  x0, #0
    b.lt 8f
    mov  x20, x0
    adrp x1, net_frame
    add  x1, x1, :lo12:net_frame
    mov  x2, #60
    mov  x3, #0
    adrp x4, sockaddr_ll
    add  x4, x4, :lo12:sockaddr_ll
    mov  x5, #20
    mov  x8, #206                /* sendto */
    svc  #0
    mov  x0, x20
    mov  x8, #57                 /* close */
    svc  #0
8:
    ret

.section .rodata
banner:
    .ascii "agentOS Linux E2E init\nagentos-linux login: "
banner_end:
.equ banner_len, banner_end - banner
prompt:
    .ascii "agentos-linux login: "
prompt_end:
.equ prompt_len, prompt_end - prompt
dev_console:
    .asciz "/dev/console"
stress_begin:
    .ascii "\nAOS_STRESS_BEGIN\n"
.equ stress_begin_len, . - stress_begin
stress_end:
    .ascii "\nAOS_STRESS_END\n"
.equ stress_end_len, . - stress_end

.section .data
.balign 8
ifreq:
    .asciz "eth0"
    .zero 11
    .hword 1                     /* IFF_UP */
    .zero 22
sockaddr_ll:
    .hword 17                    /* AF_PACKET */
    .byte 0x88, 0xb5            /* protocol in network byte order */
    .word 2                      /* eth0: loopback is ifindex 1 */
    .hword 1                     /* ARPHRD_ETHER */
    .byte 0
    .byte 6
    .byte 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0
net_frame:
    .byte 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    .byte 0x02, 0x00, 0x00, 0x00, 0x00, 0x01
    .byte 0x88, 0xb5
    .ascii "agentOS virtio-net guest proof"
    .zero 17

.section .bss
.balign 16
inbuf:
    .skip 1
.balign 16
stress_payload:
    .skip 0x40000
"#;

fn archive_entries(archive: &Path) -> anyhow::Result<Vec<String>> {
    let out = std::process::Command::new("bsdtar")
        .arg("-tf")
        .arg(archive)
        .output()
        .context("failed to run bsdtar")?;
    anyhow::ensure!(
        out.status.success(),
        "bsdtar failed listing {}",
        archive.display()
    );
    let stdout = String::from_utf8(out.stdout).context("bsdtar listing was not utf-8")?;
    Ok(stdout.lines().map(|line| line.to_string()).collect())
}

fn extract_arm64_elf_image(iso: &Path, member: &str, kernel_dest: &Path) -> anyhow::Result<()> {
    let source_id = source_file_identity(iso)?;
    let source_stamp = PathBuf::from(format!("{}.source", kernel_dest.display()));
    if kernel_dest.exists()
        && fs::metadata(kernel_dest).map(|m| m.len()).unwrap_or(0) > 0
        && fs::read_to_string(&source_stamp).unwrap_or_default() == source_id
    {
        println!(
            "[fetch-guest] arm64 ELF image already extracted: {}",
            kernel_dest.display()
        );
        return Ok(());
    }

    let tmp_root = build_tmp_dir()?;
    let tmp_dir = tempfile::Builder::new()
        .prefix("agentos-arm64-elf-image-")
        .tempdir_in(&tmp_root)
        .context("failed to create arm64 ELF image tempdir under _build/tmp")?;
    let elf = tmp_dir.path().join("kernel.elf");
    let payload = tmp_dir.path().join("kernel.payload");
    extract_iso_file(iso, member, &elf)?;

    let objcopy = find_tool(&[
        "llvm-objcopy",
        "/opt/homebrew/opt/llvm/bin/llvm-objcopy",
        "/opt/homebrew/opt/llvm@22/bin/llvm-objcopy",
        "/opt/homebrew/opt/llvm@21/bin/llvm-objcopy",
        "/usr/local/opt/llvm/bin/llvm-objcopy",
        "/usr/bin/llvm-objcopy",
    ])?;
    let status = std::process::Command::new(&objcopy)
        .args(["-O", "binary"])
        .arg(&elf)
        .arg(&payload)
        .status()
        .with_context(|| format!("failed to run {}", objcopy.display()))?;
    anyhow::ensure!(status.success(), "{} failed", objcopy.display());

    let image_size = arm64_elf_image_size(&elf)?;
    let payload =
        fs::read(&payload).with_context(|| format!("failed to read {}", payload.display()))?;
    let mut image = vec![0u8; 0x800];
    wr32(&mut image, 0x00, 0x14000200); /* branch to first instruction at +0x800 */
    wr32(&mut image, 0x04, 0);
    wr64(&mut image, 0x08, 0); /* text_offset: loader places at RAM base */
    wr64(&mut image, 0x10, image_size);
    wr64(&mut image, 0x18, 0x8); /* arm64 Image flags: little-endian, 4 KiB pages */
    image[0x38..0x3c].copy_from_slice(b"ARMd");
    image.extend_from_slice(&payload);
    write_output(kernel_dest, &image)?;
    write_output(&source_stamp, source_id.as_bytes())?;
    println!(
        "[fetch-guest] Built arm64 Image wrapper from ELF -> {}",
        kernel_dest.display()
    );
    Ok(())
}

fn stage_local_iso(
    output_dir: &Path,
    source_name: &str,
    dest_name: &str,
    source_url: &str,
) -> anyhow::Result<PathBuf> {
    let dest = output_dir.join(dest_name);
    if staged_regular_iso_ready(&dest)? {
        println!("[fetch-guest] ISO already staged: {}", dest.display());
        return Ok(dest);
    }

    let src = ensure_cached_iso(source_name, source_url)?;
    if staged_symlink_ready(&dest, &src)? {
        println!("[fetch-guest] ISO already staged: {}", dest.display());
        return Ok(dest);
    }

    fs::create_dir_all(output_dir)
        .with_context(|| format!("failed to create {}", output_dir.display()))?;

    if std::env::var(COPY_ISOS_ENV).ok().as_deref() == Some("1") {
        println!(
            "[fetch-guest] Copying {} -> {}",
            src.display(),
            dest.display()
        );
        fs::copy(&src, &dest)
            .with_context(|| format!("failed to copy {} to {}", src.display(), dest.display()))?;
    } else {
        println!(
            "[fetch-guest] Staging ISO symlink {} -> {}",
            dest.display(),
            src.display()
        );
        symlink_file(&src, &dest).with_context(|| {
            format!("failed to symlink {} to {}", dest.display(), src.display())
        })?;
    }

    Ok(dest)
}

fn source_override(staged: &Path, variables: Option<&str>) -> anyhow::Result<Option<PathBuf>> {
    let Some(variables) = variables else {
        return Ok(None);
    };
    for env in variables
        .split(',')
        .map(str::trim)
        .filter(|name| !name.is_empty())
    {
        anyhow::ensure!(
            env.bytes()
                .all(|byte| byte.is_ascii_uppercase() || byte.is_ascii_digit() || byte == b'_'),
            "invalid override environment variable name {env:?}"
        );
        let Some(value) = std::env::var_os(env) else {
            continue;
        };
        if value.is_empty() {
            continue;
        }

        let path = PathBuf::from(value);
        if path == staged {
            continue;
        }
        if path.exists() && staged.exists() {
            let src_canon = path.canonicalize().ok();
            let staged_canon = staged.canonicalize().ok();
            if src_canon.is_some() && src_canon == staged_canon {
                continue;
            }
        }

        anyhow::ensure!(
            path.exists(),
            "{} points to missing guest image {}",
            env,
            path.display()
        );
        anyhow::ensure!(
            fs::metadata(&path).map(|m| m.len()).unwrap_or(0) > 0,
            "{} points to empty guest image {}",
            env,
            path.display()
        );
        return Ok(Some(path));
    }
    Ok(None)
}

fn stage_existing_iso(output_dir: &Path, src: &Path, dest_name: &str) -> anyhow::Result<PathBuf> {
    let dest = output_dir.join(dest_name);
    let src = src
        .canonicalize()
        .with_context(|| format!("failed to resolve {}", src.display()))?;

    if fs::symlink_metadata(&dest).is_ok() {
        if let Ok(existing) = dest.canonicalize() {
            if existing == src && fs::metadata(&dest).map(|m| m.len()).unwrap_or(0) > 0 {
                println!("[fetch-guest] ISO already staged: {}", dest.display());
                return Ok(dest);
            }
        }
        fs::remove_file(&dest)
            .with_context(|| format!("failed to remove stale ISO stage {}", dest.display()))?;
    }

    fs::create_dir_all(output_dir)
        .with_context(|| format!("failed to create {}", output_dir.display()))?;

    if std::env::var(COPY_ISOS_ENV).ok().as_deref() == Some("1") {
        println!(
            "[fetch-guest] Copying {} -> {}",
            src.display(),
            dest.display()
        );
        fs::copy(&src, &dest)
            .with_context(|| format!("failed to copy {} to {}", src.display(), dest.display()))?;
    } else {
        println!(
            "[fetch-guest] Staging ISO symlink {} -> {}",
            dest.display(),
            src.display()
        );
        symlink_file(&src, &dest).with_context(|| {
            format!("failed to symlink {} to {}", dest.display(), src.display())
        })?;
    }

    Ok(dest)
}

fn source_file_identity(path: &Path) -> anyhow::Result<String> {
    let resolved = path
        .canonicalize()
        .unwrap_or_else(|_| path.to_path_buf())
        .display()
        .to_string();
    let meta = fs::metadata(path)
        .with_context(|| format!("failed to inspect source file {}", path.display()))?;
    let modified = meta.modified().unwrap_or(UNIX_EPOCH);
    let modified = modified.duration_since(UNIX_EPOCH).unwrap_or_default();
    Ok(format!(
        "path={resolved}\nlen={}\nmtime_ns={}\n",
        meta.len(),
        modified.as_nanos()
    ))
}

fn staged_regular_iso_ready(dest: &Path) -> anyhow::Result<bool> {
    match fs::symlink_metadata(dest) {
        Ok(meta) if !meta.file_type().is_symlink() => {
            if meta.len() > 0 {
                return Ok(true);
            }
            fs::remove_file(dest)
                .with_context(|| format!("failed to remove stale ISO stage {}", dest.display()))?;
            Ok(false)
        }
        Ok(_) => Ok(false),
        Err(err) if err.kind() == ErrorKind::NotFound => Ok(false),
        Err(err) => {
            Err(err).with_context(|| format!("failed to inspect staged ISO {}", dest.display()))
        }
    }
}

fn staged_symlink_ready(dest: &Path, src: &Path) -> anyhow::Result<bool> {
    match fs::symlink_metadata(dest) {
        Ok(meta) if meta.file_type().is_symlink() => {
            let target = fs::read_link(dest)
                .with_context(|| format!("failed to read staged ISO link {}", dest.display()))?;
            if target == src && fs::metadata(dest).map(|m| m.len()).unwrap_or(0) > 0 {
                return Ok(true);
            }
            fs::remove_file(dest).with_context(|| {
                format!("failed to remove stale ISO symlink {}", dest.display())
            })?;
            Ok(false)
        }
        Ok(_) => Ok(false),
        Err(err) if err.kind() == ErrorKind::NotFound => Ok(false),
        Err(err) => {
            Err(err).with_context(|| format!("failed to inspect staged ISO {}", dest.display()))
        }
    }
}

#[cfg(unix)]
fn symlink_file(src: &Path, dest: &Path) -> std::io::Result<()> {
    std::os::unix::fs::symlink(src, dest)
}

#[cfg(not(unix))]
fn symlink_file(src: &Path, dest: &Path) -> std::io::Result<()> {
    fs::copy(src, dest).map(|_| ())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn native_helper_recipe_builds_reproducible_static_elf_for_both_architectures() {
        let dir = tempfile::tempdir().unwrap();
        fs::write(
            dir.path().join("helper.c"),
            b"void _start(void) { for (;;) {} }\n",
        )
        .unwrap();
        for (architecture, machine) in [("x86_64", 62u16), ("aarch64", 183u16)] {
            let step = RecipeStep {
                action: "build-static-linux-elf".into(),
                args: [
                    ("source", "helper.c"),
                    ("output", "helper"),
                    ("architecture", architecture),
                ]
                .into_iter()
                .map(|(k, v)| (k.into(), v.into()))
                .collect(),
            };
            build_static_linux_elf(&step, dir.path(), dir.path()).unwrap();
            let first = fs::read(dir.path().join("helper")).unwrap();
            assert_eq!(&first[..4], b"\x7fELF");
            assert_eq!(first[4], 2); // ELF64
            assert_eq!(u16::from_le_bytes([first[16], first[17]]), 2); // executable
            assert_eq!(u16::from_le_bytes([first[18], first[19]]), machine);
            build_static_linux_elf(&step, dir.path(), dir.path()).unwrap();
            assert_eq!(fs::read(dir.path().join("helper")).unwrap(), first);
            fs::write(
                dir.path().join("helper.c"),
                b"#error intentional build failure\n",
            )
            .unwrap();
            assert!(build_static_linux_elf(&step, dir.path(), dir.path()).is_err());
            assert_eq!(fs::read(dir.path().join("helper")).unwrap(), first);
            fs::write(
                dir.path().join("helper.c"),
                b"void _start(void) { for (;;) {} }\n",
            )
            .unwrap();
        }
    }

    #[test]
    fn binary_initramfs_recipe_preserves_bytes_and_rejects_changed_payload() {
        let dir = tempfile::tempdir().unwrap();
        let payload = b"\x7fELF\0\xff\x80\n";
        fs::write(dir.path().join("helper"), payload).unwrap();
        fs::write(dir.path().join("base"), b"old").unwrap();
        let mut step = RecipeStep {
            action: "append-initramfs-file".into(),
            args: [
                ("source", "base".into()),
                ("output", "ready".into()),
                ("path", "init".into()),
                ("mode", "0755".into()),
                ("content_file", "helper".into()),
                ("content_sha256", format!("{:x}", Sha256::digest(payload))),
            ]
            .into_iter()
            .map(|(k, v)| (k.into(), v))
            .collect(),
        };
        execute_acquire_step(&step, dir.path()).unwrap();
        let bytes = fs::read(dir.path().join("ready")).unwrap();
        assert_eq!(&bytes[..4], b"old\0");
        let archive = &bytes[4..];
        assert_eq!(&archive[..6], b"070701");
        assert_eq!(&archive[14..22], b"000081ed");
        assert_eq!(&archive[54..62], b"00000008");
        assert_eq!(&archive[110..115], b"init\0");
        assert_eq!(&archive[116..124], payload);
        fs::write(dir.path().join("helper"), b"changed").unwrap();
        assert!(execute_acquire_step(&step, dir.path()).is_err());
        assert_eq!(fs::read(dir.path().join("ready")).unwrap(), bytes);
        step.args.insert("content".into(), "ambiguous".into());
        assert!(execute_acquire_step(&step, dir.path()).is_err());
        step.args.remove("content");
        step.args.remove("content_sha256");
        assert!(execute_acquire_step(&step, dir.path()).is_err());
        step.args.insert("content_sha256".into(), "00".repeat(32));
        step.args.insert("content_file".into(), "../helper".into());
        assert!(execute_acquire_step(&step, dir.path()).is_err());
    }

    #[test]
    fn initramfs_overlay_is_aligned_bounded_newc() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("base.initrd");
        let dest = dir.path().join("ready.initrd");
        fs::write(&source, b"base!").unwrap();

        append_initramfs_file(
            &source,
            &dest,
            "scripts/casper-bottom/16console",
            "0755",
            b"#!/bin/sh\nexit 0\n",
            None,
        )
        .unwrap();

        let bytes = fs::read(&dest).unwrap();
        assert_eq!(&bytes[..5], b"base!");
        assert_eq!(&bytes[5..8], &[0, 0, 0]);
        assert_eq!(&bytes[8..14], b"070701");
        let overlay = String::from_utf8_lossy(&bytes[8..]);
        assert!(overlay.contains("scripts/casper-bottom/16console"));
        assert!(overlay.contains("#!/bin/sh\nexit 0\n"));
        assert!(overlay.contains("TRAILER!!!"));
    }

    #[test]
    fn standalone_initramfs_file_contains_only_the_bounded_overlay() {
        let dir = tempfile::tempdir().unwrap();
        let dest = dir.path().join("overlay.initrd");

        build_initramfs_file(
            &dest,
            "scripts/casper-bottom/25configure_init",
            "0755",
            b"#!/bin/sh\necho ready\n",
            None,
        )
        .unwrap();

        let bytes = fs::read(&dest).unwrap();
        assert_eq!(&bytes[..6], b"070701");
        let archive = String::from_utf8_lossy(&bytes);
        assert!(archive.contains("scripts/casper-bottom/25configure_init"));
        assert!(archive.contains("#!/bin/sh\necho ready\n"));
        assert!(archive.contains("TRAILER!!!"));
    }

    #[test]
    fn initramfs_overlay_rejects_escaping_paths_and_modes() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("base.initrd");
        let dest = dir.path().join("ready.initrd");
        fs::write(&source, b"base").unwrap();
        assert!(append_initramfs_file(&source, &dest, "../init", "0755", b"x", None).is_err());
        assert!(append_initramfs_file(&source, &dest, "init", "4755", b"x", None).is_err());
    }

    #[test]
    fn initramfs_module_filter_retains_exact_selection_and_trailing_archive() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("full.initrd");
        let dest = dir.path().join("filtered.initrd");
        let mut archive = Vec::new();
        append_newc_file(
            &mut archive,
            "usr/lib/modules/1/kernel/net/virtio_net.ko.zst",
            1,
            0o644,
            b"net-module",
        )
        .unwrap();
        append_newc_file(
            &mut archive,
            "usr/lib/modules/1/kernel/gpu/unrelated.ko.zst",
            2,
            0o644,
            b"unrelated-module",
        )
        .unwrap();
        append_newc_trailer(&mut archive, 3).unwrap();
        let trailing = b"\xfd7zXZ\0trailing-archive";
        archive.extend_from_slice(trailing);
        fs::write(&source, archive).unwrap();

        filter_initramfs_modules(&source, &dest, "virtio_net").unwrap();
        let filtered = fs::read(&dest).unwrap();
        let text = String::from_utf8_lossy(&filtered);
        assert!(text.contains("virtio_net.ko.zst"));
        assert!(!text.contains("unrelated.ko.zst"));
        assert!(filtered.ends_with(trailing));
        assert!(filter_initramfs_modules(&source, &dest, "missing").is_err());
        assert!(filter_initramfs_modules(&source, &dest, "virtio_net,virtio_net").is_err());
    }

    #[test]
    fn zstd_initramfs_overlay_is_a_separate_reproducible_frame() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("base.initrd");
        let dest = dir.path().join("ready.initrd");
        fs::write(&source, b"base-zstd-frame").unwrap();
        append_initramfs_file(&source, &dest, "init", "0755", b"payload", Some("zstd")).unwrap();

        let bytes = fs::read(&dest).unwrap();
        let compressed = &bytes[b"base-zstd-frame".len()..];
        assert_eq!(&compressed[..4], &[0x28, 0xb5, 0x2f, 0xfd]);
        let overlay = zstd::stream::decode_all(compressed).unwrap();
        let overlay = String::from_utf8_lossy(&overlay);
        assert!(overlay.contains("init"));
        assert!(overlay.contains("payload"));
        assert!(overlay.contains("TRAILER!!!"));
    }

    #[test]
    fn sha512_verification_accepts_exact_content_and_rejects_drift() {
        let dir = tempfile::tempdir().unwrap();
        let artifact = dir.path().join("artifact");
        fs::write(&artifact, b"agentOS\n").unwrap();
        let expected = format!("{:x}", Sha512::digest(b"agentOS\n"));
        verify_sha512(&artifact, &expected).unwrap();
        assert!(verify_sha512(&artifact, &"0".repeat(128)).is_err());
    }

    #[test]
    fn sha256_verification_accepts_exact_content_and_rejects_drift() {
        let dir = tempfile::tempdir().unwrap();
        let artifact = dir.path().join("artifact");
        fs::write(&artifact, b"agentOS\n").unwrap();
        let expected = format!("{:x}", Sha256::digest(b"agentOS\n"));
        verify_sha256(&artifact, &expected).unwrap();
        assert!(verify_sha256(&artifact, &"0".repeat(64)).is_err());
    }

    #[test]
    fn qcow2_conversion_produces_raw_image_and_reuses_matching_source() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("source.qcow2");
        let dest = dir.path().join("dest.raw");
        let Ok(qemu_img) = find_tool(&[
            "qemu-img",
            "/opt/homebrew/bin/qemu-img",
            "/usr/local/bin/qemu-img",
            "/usr/bin/qemu-img",
        ]) else {
            eprintln!("skipping: qemu-img not installed on this host");
            return;
        };
        let status = std::process::Command::new(qemu_img)
            .args(["create", "-f", "qcow2"])
            .arg(&source)
            .arg("1M")
            .status()
            .unwrap();
        assert!(status.success());

        convert_qcow2_to_raw(&source, &dest).unwrap();
        assert_eq!(fs::metadata(&dest).unwrap().len(), 1024 * 1024);
        let stamp = PathBuf::from(format!("{}.source", dest.display()));
        assert_eq!(
            fs::read_to_string(stamp).unwrap(),
            source_file_identity(&source).unwrap()
        );
        convert_qcow2_to_raw(&source, &dest).unwrap();
        assert_eq!(fs::metadata(&dest).unwrap().len(), 1024 * 1024);
    }

    #[test]
    fn gpt_partition_extraction_uses_declared_entry_and_exact_bounds() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("disk.raw");
        let dest = dir.path().join("partition.raw");
        let mut disk = vec![0u8; 64 * 512];
        disk[512..520].copy_from_slice(b"EFI PART");
        disk[524..528].copy_from_slice(&92u32.to_le_bytes());
        disk[584..592].copy_from_slice(&2u64.to_le_bytes());
        disk[592..596].copy_from_slice(&4u32.to_le_bytes());
        disk[596..600].copy_from_slice(&128u32.to_le_bytes());
        let entry = 2 * 512;
        disk[entry] = 1;
        disk[entry + 32..entry + 40].copy_from_slice(&8u64.to_le_bytes());
        disk[entry + 40..entry + 48].copy_from_slice(&11u64.to_le_bytes());
        disk[8 * 512..8 * 512 + 16].copy_from_slice(b"agentOS-GPT-data");
        fs::write(&source, disk).unwrap();

        extract_gpt_partition(&source, &dest, "1", None).unwrap();
        let partition = fs::read(&dest).unwrap();
        assert_eq!(partition.len(), 4 * 512);
        assert_eq!(&partition[..16], b"agentOS-GPT-data");
        assert!(extract_gpt_partition(&source, &dir.path().join("unused"), "2", None).is_err());
    }

    #[test]
    fn gpt_ext4_configuration_is_private_verified_and_preserves_writable_disk() {
        let Ok(mkfs) = find_tool(&[
            "mkfs.ext4",
            "/usr/sbin/mkfs.ext4",
            "/opt/homebrew/opt/e2fsprogs/sbin/mkfs.ext4",
        ]) else {
            eprintln!("skipping: mkfs.ext4 is unavailable");
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let partition = dir.path().join("root.ext4");
        let length = 8 * 1024 * 1024;
        fs::File::create(&partition)
            .unwrap()
            .set_len(length as u64)
            .unwrap();
        assert!(std::process::Command::new(mkfs)
            .args(["-q", "-F", "-O", "^has_journal"])
            .arg(&partition)
            .status()
            .unwrap()
            .success());
        let offset = 1024 * 1024;
        let mut original = vec![0x5au8; offset + length + 512];
        original[512..520].copy_from_slice(b"EFI PART");
        original[524..528].copy_from_slice(&92u32.to_le_bytes());
        original[584..592].copy_from_slice(&2u64.to_le_bytes());
        original[592..596].copy_from_slice(&1u32.to_le_bytes());
        original[596..600].copy_from_slice(&128u32.to_le_bytes());
        original[1056..1064].copy_from_slice(&(offset as u64 / 512).to_le_bytes());
        original[1064..1072].copy_from_slice(&((offset + length) as u64 / 512 - 1).to_le_bytes());
        original[offset..offset + length].copy_from_slice(&fs::read(&partition).unwrap());
        let source = dir.path().join("base.raw");
        let output = dir.path().join("configured.raw");
        fs::write(&source, &original).unwrap();
        let path = "/etc/systemd/system/ssh.service.d/agentos-hostkeys.conf";
        let config = b"[Service]\nExecStartPre=/usr/bin/ssh-keygen -A\n";
        install_gpt_ext4_file(&source, &output, "1", None, path, config).unwrap();
        assert_eq!(
            fs::read(&source).unwrap(),
            original,
            "base image must not change"
        );
        let configured = fs::read(&output).unwrap();
        assert_eq!(&configured[..offset], &original[..offset]);
        assert_eq!(&configured[offset + length..], &original[offset + length..]);
        let extracted = dir.path().join("configured.ext4");
        extract_gpt_partition(&output, &extracted, "1", None).unwrap();
        let readback = dir.path().join("config");
        extract_ext4_file(&extracted, &readback, path).unwrap();
        assert_eq!(fs::read(&readback).unwrap(), config);
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                fs::metadata(&readback).unwrap().permissions().mode() & 0o777,
                0o644
            );
        }
        let before = fs::metadata(&output).unwrap().modified().unwrap();
        install_gpt_ext4_file(&source, &output, "1", None, path, config).unwrap();
        assert_eq!(fs::metadata(&output).unwrap().modified().unwrap(), before);
        assert!(install_gpt_ext4_file(&source, &output, "1", None, path, b"changed").is_err());
        assert_eq!(
            fs::read(&output).unwrap(),
            configured,
            "recipe changes must not erase guest data"
        );
        assert!(install_gpt_ext4_file(&source, &source, "1", None, path, config).is_err());
        assert!(install_gpt_ext4_file(
            &source,
            &dir.path().join("bad.raw"),
            "1",
            None,
            "/etc/../bad",
            config
        )
        .is_err());
        assert!(!dir.path().join("bad.raw").exists());
    }

    #[test]
    fn arm64_image_normalization_accepts_a_raw_image() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("vmlinuz");
        let dest = dir.path().join("Image");
        let mut image = vec![0u8; 0x1000];
        image[0x38..0x3c].copy_from_slice(b"ARMd");
        fs::write(&source, &image).unwrap();
        normalize_arm64_linux_image(&source, &dest).unwrap();
        assert_eq!(fs::read(dest).unwrap(), image);
    }

    #[test]
    fn iso_dir_prefers_explicit_agentos_iso_dir() {
        let dir = iso_dir_from_env(
            Some(OsString::from("/tmp/agentos-isos")),
            Some(OsString::from("/tmp/xdg-cache")),
            Some(OsString::from("/tmp/home")),
        );
        assert_eq!(dir, PathBuf::from("/tmp/agentos-isos"));
    }

    #[test]
    fn iso_dir_defaults_to_xdg_cache() {
        let dir = iso_dir_from_env(
            None,
            Some(OsString::from("/tmp/xdg-cache")),
            Some(OsString::from("/tmp/home")),
        );
        assert_eq!(dir, PathBuf::from("/tmp/xdg-cache/agentos/isos"));
    }

    #[test]
    fn iso_dir_defaults_to_home_cache_without_xdg() {
        let dir = iso_dir_from_env(None, None, Some(OsString::from("/tmp/home")));
        assert_eq!(dir, PathBuf::from("/tmp/home/.cache/agentos/isos"));
    }
}

fn extract_iso_file(iso: &Path, entry: &str, dest: &Path) -> anyhow::Result<()> {
    if dest.exists() && fs::metadata(dest).map(|m| m.len()).unwrap_or(0) > 0 {
        println!(
            "[fetch-guest] ISO member already extracted: {}",
            dest.display()
        );
        return Ok(());
    }

    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("failed to create {}", parent.display()))?;
    }
    let tmp = dest.with_extension("tmp");
    let _ = fs::remove_file(&tmp);
    let out =
        fs::File::create(&tmp).with_context(|| format!("failed to create {}", tmp.display()))?;

    println!("[fetch-guest] Extracting {} from {}", entry, iso.display());
    let status = std::process::Command::new("bsdtar")
        .arg("-xOf")
        .arg(iso)
        .arg(entry)
        .stdout(Stdio::from(out))
        .status()
        .context("failed to run bsdtar")?;
    anyhow::ensure!(status.success(), "bsdtar failed extracting {}", entry);
    anyhow::ensure!(
        fs::metadata(&tmp).map(|m| m.len()).unwrap_or(0) > 0,
        "extracted {} was empty",
        entry
    );
    fs::rename(&tmp, dest)
        .with_context(|| format!("failed to move {} to {}", tmp.display(), dest.display()))?;
    Ok(())
}

fn extract_arm64_linux_image(iso: &Path, member: &str, kernel_dest: &Path) -> anyhow::Result<()> {
    if kernel_dest.exists() && fs::metadata(kernel_dest).map(|m| m.len()).unwrap_or(0) > 0 {
        println!(
            "[fetch-guest] arm64 Linux Image already extracted: {}",
            kernel_dest.display()
        );
        return Ok(());
    }

    let tmp_root = build_tmp_dir()?;
    let tmp_dir = tempfile::Builder::new()
        .prefix("agentos-arm64-linux-image-")
        .tempdir_in(&tmp_root)
        .context("failed to create arm64 Linux Image tempdir under _build/tmp")?;
    let vmlinuz = tmp_dir.path().join("vmlinuz");
    extract_iso_file(iso, member, &vmlinuz)?;
    decode_arm64_kernel(&vmlinuz, kernel_dest, tmp_dir.path())
}

fn normalize_arm64_linux_image(source: &Path, kernel_dest: &Path) -> anyhow::Result<()> {
    anyhow::ensure!(
        source != kernel_dest,
        "arm64 kernel source and output must differ"
    );
    let source_id = source_file_identity(source)?;
    let source_stamp = PathBuf::from(format!("{}.source", kernel_dest.display()));
    if kernel_dest.is_file()
        && fs::metadata(kernel_dest)
            .map(|meta| meta.len())
            .unwrap_or(0)
            > 0
        && fs::read_to_string(&source_stamp).unwrap_or_default() == source_id
    {
        println!(
            "[fetch-guest] arm64 Linux Image already normalized: {}",
            kernel_dest.display()
        );
        return Ok(());
    }
    let tmp_root = build_tmp_dir()?;
    let tmp_dir = tempfile::Builder::new()
        .prefix("agentos-arm64-linux-image-")
        .tempdir_in(&tmp_root)
        .context("failed to create arm64 Linux Image tempdir under _build/tmp")?;
    decode_arm64_kernel(source, kernel_dest, tmp_dir.path())?;
    write_output(&source_stamp, source_id.as_bytes())?;
    Ok(())
}

fn decode_arm64_kernel(vmlinuz: &Path, kernel_dest: &Path, work_dir: &Path) -> anyhow::Result<()> {
    let bytes =
        fs::read(vmlinuz).with_context(|| format!("failed to read {}", vmlinuz.display()))?;

    if is_raw_arm64_image(&bytes) {
        write_output(kernel_dest, &bytes)?;
        println!(
            "[fetch-guest] Copied raw arm64 kernel Image -> {}",
            kernel_dest.display()
        );
        return Ok(());
    }

    if bytes.starts_with(&[0x1f, 0x8b]) {
        let mut gz = flate2::read::GzDecoder::new(&bytes[..]);
        let mut raw = Vec::new();
        gz.read_to_end(&mut raw)
            .context("failed to decompress gzip vmlinuz")?;
        anyhow::ensure!(
            is_raw_arm64_image(&raw),
            "gzip payload is not an arm64 Image"
        );
        write_output(kernel_dest, &raw)?;
        println!(
            "[fetch-guest] Decompressed gzip arm64 kernel Image -> {}",
            kernel_dest.display()
        );
        return Ok(());
    }

    if bytes.starts_with(b"MZ") {
        let linux_section = work_dir.join("vmlinuz.linux");
        let objcopy = find_tool(&[
            "llvm-objcopy",
            "/opt/homebrew/opt/llvm/bin/llvm-objcopy",
            "/opt/homebrew/opt/llvm@22/bin/llvm-objcopy",
            "/opt/homebrew/opt/llvm@21/bin/llvm-objcopy",
            "/usr/local/opt/llvm/bin/llvm-objcopy",
            "/usr/bin/llvm-objcopy",
        ])?;
        let status = std::process::Command::new(&objcopy)
            .arg("--dump-section")
            .arg(format!(".linux={}", linux_section.display()))
            .arg(vmlinuz)
            .status()
            .with_context(|| format!("failed to run {}", objcopy.display()))?;
        anyhow::ensure!(status.success(), "{} failed", objcopy.display());
        decode_zboot_payload(&linux_section, kernel_dest, work_dir)?;
        println!(
            "[fetch-guest] Extracted PE/zboot arm64 kernel Image -> {}",
            kernel_dest.display()
        );
        return Ok(());
    }

    anyhow::bail!(
        "{} is not a raw, gzip, or PE/zboot arm64 kernel",
        vmlinuz.display()
    );
}

fn decode_zboot_payload(
    section_path: &Path,
    kernel_dest: &Path,
    work_dir: &Path,
) -> anyhow::Result<()> {
    let section = fs::read(section_path)
        .with_context(|| format!("failed to read {}", section_path.display()))?;
    anyhow::ensure!(section.len() >= 32, "zboot section is too small");
    anyhow::ensure!(&section[4..8] == b"zimg", "missing arm64 zboot magic");

    let payload_off = u32::from_le_bytes(section[8..12].try_into().unwrap()) as usize;
    let payload_size = u32::from_le_bytes(section[12..16].try_into().unwrap()) as usize;
    let comp = &section[24..28];
    let payload_end = payload_off
        .checked_add(payload_size)
        .context("zboot payload size overflow")?;
    anyhow::ensure!(
        payload_end <= section.len(),
        "zboot payload exceeds section length"
    );
    let payload = &section[payload_off..payload_end];

    if comp == b"gzip" {
        let mut gz = flate2::read::GzDecoder::new(payload);
        let mut raw = Vec::new();
        gz.read_to_end(&mut raw)
            .context("failed to decompress zboot gzip payload")?;
        anyhow::ensure!(
            is_raw_arm64_image(&raw),
            "zboot gzip payload is not an arm64 Image"
        );
        write_output(kernel_dest, &raw)
    } else if comp == b"zstd" {
        let payload_path = work_dir.join("vmlinuz.linux.zst");
        fs::write(&payload_path, payload)
            .with_context(|| format!("failed to write {}", payload_path.display()))?;
        let tmp = kernel_dest.with_extension("tmp");
        let out = fs::File::create(&tmp)
            .with_context(|| format!("failed to create {}", tmp.display()))?;
        let zstd = find_tool(&[
            "zstd",
            "/opt/homebrew/bin/zstd",
            "/usr/local/bin/zstd",
            "/usr/bin/zstd",
        ])?;
        let status = std::process::Command::new(&zstd)
            .arg("-dc")
            .arg(&payload_path)
            .stdout(Stdio::from(out))
            .status()
            .with_context(|| format!("failed to run {}", zstd.display()))?;
        anyhow::ensure!(status.success(), "{} failed", zstd.display());
        let raw = fs::read(&tmp).with_context(|| format!("failed to read {}", tmp.display()))?;
        anyhow::ensure!(
            is_raw_arm64_image(&raw),
            "zboot zstd payload is not an arm64 Image"
        );
        fs::rename(&tmp, kernel_dest).with_context(|| {
            format!(
                "failed to move {} to {}",
                tmp.display(),
                kernel_dest.display()
            )
        })
    } else if comp == [0, 0, 0, 0] {
        write_output(kernel_dest, payload)
    } else {
        anyhow::bail!(
            "unsupported arm64 zboot compression tag {:?}",
            String::from_utf8_lossy(comp)
        );
    }
}

fn is_raw_arm64_image(bytes: &[u8]) -> bool {
    bytes.len() > 0x3c && &bytes[0x38..0x3c] == b"ARMd"
}

fn write_output(dest: &Path, bytes: &[u8]) -> anyhow::Result<()> {
    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("failed to create {}", parent.display()))?;
    }
    let tmp = dest.with_extension("tmp");
    fs::write(&tmp, bytes).with_context(|| format!("failed to write {}", tmp.display()))?;
    fs::rename(&tmp, dest)
        .with_context(|| format!("failed to move {} to {}", tmp.display(), dest.display()))?;
    Ok(())
}

fn arm64_elf_image_size(elf_path: &Path) -> anyhow::Result<u64> {
    let elf =
        fs::read(elf_path).with_context(|| format!("failed to read {}", elf_path.display()))?;
    anyhow::ensure!(elf.len() >= 64, "arm64 kernel ELF is too small");
    anyhow::ensure!(&elf[0..4] == b"\x7fELF", "arm64 kernel is not an ELF file");
    anyhow::ensure!(
        elf[4] == 2 && elf[5] == 1,
        "arm64 kernel is not ELF64 little-endian"
    );

    let phoff = rd64(&elf, 32)? as usize;
    let phentsize = rd16(&elf, 54)? as usize;
    let phnum = rd16(&elf, 56)? as usize;
    anyhow::ensure!(phentsize >= 56, "unexpected ELF program header size");

    let mut base = u64::MAX;
    let mut end = 0u64;
    for idx in 0..phnum {
        let off = phoff
            .checked_add(
                idx.checked_mul(phentsize)
                    .context("ELF phdr offset overflow")?,
            )
            .context("ELF phdr offset overflow")?;
        anyhow::ensure!(
            off + phentsize <= elf.len(),
            "ELF program header is truncated"
        );
        let p_type = rd32(&elf, off)?;
        if p_type != 1 {
            continue;
        }
        let vaddr = rd64(&elf, off + 16)?;
        let memsz = rd64(&elf, off + 40)?;
        base = base.min(vaddr);
        end = end.max(vaddr.checked_add(memsz).context("ELF LOAD end overflow")?);
    }

    anyhow::ensure!(
        base != u64::MAX && end > base,
        "arm64 ELF has no LOAD segments"
    );
    Ok(align_up(end - base, 0x1000))
}

fn rd16(buf: &[u8], off: usize) -> anyhow::Result<u16> {
    anyhow::ensure!(off + 2 <= buf.len(), "read past end of buffer");
    Ok(u16::from_le_bytes(buf[off..off + 2].try_into().unwrap()))
}

fn rd32(buf: &[u8], off: usize) -> anyhow::Result<u32> {
    anyhow::ensure!(off + 4 <= buf.len(), "read past end of buffer");
    Ok(u32::from_le_bytes(buf[off..off + 4].try_into().unwrap()))
}

fn rd64(buf: &[u8], off: usize) -> anyhow::Result<u64> {
    anyhow::ensure!(off + 8 <= buf.len(), "read past end of buffer");
    Ok(u64::from_le_bytes(buf[off..off + 8].try_into().unwrap()))
}

fn wr32(buf: &mut [u8], off: usize, value: u32) {
    buf[off..off + 4].copy_from_slice(&value.to_le_bytes());
}

fn wr64(buf: &mut [u8], off: usize, value: u64) {
    buf[off..off + 8].copy_from_slice(&value.to_le_bytes());
}

fn align_up(value: u64, align: u64) -> u64 {
    (value + align - 1) & !(align - 1)
}

fn find_tool(candidates: &[&str]) -> anyhow::Result<PathBuf> {
    for candidate in candidates {
        let path = PathBuf::from(candidate);
        if path.is_absolute() && path.exists() {
            return Ok(path);
        }

        if !candidate.contains('/') {
            let out = std::process::Command::new("sh")
                .args(["-c", &format!("command -v {}", candidate)])
                .output()
                .with_context(|| format!("failed to search for {}", candidate))?;
            if out.status.success() {
                let found = String::from_utf8(out.stdout)
                    .context("command -v output was not utf-8")?
                    .trim()
                    .to_string();
                if !found.is_empty() {
                    return Ok(PathBuf::from(found));
                }
            }
        }
    }

    anyhow::bail!("required tool not found; tried {:?}", candidates);
}
