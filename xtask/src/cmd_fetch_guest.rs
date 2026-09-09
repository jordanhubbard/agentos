use crate::cmd_guest_profile::{self, RecipeStep};
use crate::FetchGuestArgs;
use anyhow::Context;
use std::ffi::OsString;
use std::fs;
use std::io::{ErrorKind, Read};
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
    let dir = repo_root()?.join("build/tmp");
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
        .context("failed to create guest archive tempdir under build/tmp")?;
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
        other => anyhow::bail!("host acquire action {other:?} is not executable"),
    }
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
        .context("failed to create Linux probe initrd tempdir under build/tmp")?;
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
            .any(|entry| entry == "agentos-init-v3" || entry == "./agentos-init-v3"))
}

fn build_linux_e2e_init(work_dir: &Path) -> anyhow::Result<Vec<u8>> {
    let init_s = work_dir.join("agentos-linux-e2e-init.S");
    let init_elf = work_dir.join("init");
    fs::write(&init_s, LINUX_E2E_INIT_ASM)
        .with_context(|| format!("failed to write {}", init_s.display()))?;

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
            "aarch64-linux-gnu",
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

    let init =
        fs::read(&init_elf).with_context(|| format!("failed to read {}", init_elf.display()))?;
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
        "agentos-init-v3",
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
    cmp  w3, #10
    b.ne 1b

    mov  x0, #1
    adrp x1, prompt
    add  x1, x1, :lo12:prompt
    mov  x2, #prompt_len
    mov  x8, #64
    svc  #0
    b 1b

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
        .context("failed to create arm64 ELF image tempdir under build/tmp")?;
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
        .context("failed to create arm64 Linux Image tempdir under build/tmp")?;
    let vmlinuz = tmp_dir.path().join("vmlinuz");
    extract_iso_file(iso, member, &vmlinuz)?;
    decode_arm64_kernel(&vmlinuz, kernel_dest, tmp_dir.path())
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
