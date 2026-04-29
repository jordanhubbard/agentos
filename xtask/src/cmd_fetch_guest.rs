use crate::{FetchGuestArgs, GuestOs};
use anyhow::Context;
use indicatif::{ProgressBar, ProgressStyle};
use std::io::Write;
use std::path::{Path, PathBuf};

const UBUNTU_VERSION: &str = "24.04";
const UBUNTU_CLOUD_IMG: &str = "noble-server-cloudimg-arm64.img";
const UBUNTU_BASE_URL: &str = "https://cloud-images.ubuntu.com/noble/current";
const UBUNTU_RAW_NAME: &str = "ubuntu-24.04-aarch64.img";

/* Ubuntu Noble arm64 package repository — used for kernel .deb extraction */
const UBUNTU_PORTS_BASE: &str = "https://ports.ubuntu.com/ubuntu-ports";
const UBUNTU_PKGS_INDEX: &str = "dists/noble/main/binary-arm64/Packages.gz";

/* The extracted kernel is stored here (relative to repo root).
 * vmm.mk looks for UBUNTU_KERNEL at this path. */
const UBUNTU_KERNEL_RELPATH: &str = "guest-images/ubuntu-kernel-6.8.0-Image";

const FREEBSD_VERSION: &str = "14.4";
const FREEBSD_BASE_URL: &str =
    "https://download.freebsd.org/releases/VM-IMAGES/14.4-RELEASE/aarch64/Latest";
const FREEBSD_IMAGE_XZ: &str = "FreeBSD-14.4-RELEASE-arm64-aarch64-ufs.qcow2.xz";
const FREEBSD_RAW_NAME: &str = "freebsd-14.4-aarch64.img";

fn default_image_dir() -> anyhow::Result<PathBuf> {
    let home = std::env::var("HOME").context("HOME environment variable not set")?;
    Ok(PathBuf::from(home).join(".local/agentos-images"))
}

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

pub fn run(args: &FetchGuestArgs) -> anyhow::Result<()> {
    let output_dir = match &args.output_dir {
        Some(d) => PathBuf::from(d),
        None => default_image_dir()?,
    };
    std::fs::create_dir_all(&output_dir)
        .with_context(|| format!("failed to create output dir: {}", output_dir.display()))?;

    match args.os {
        GuestOs::Ubuntu => fetch_ubuntu(&output_dir),
        GuestOs::Freebsd => fetch_freebsd(&output_dir),
    }
}

fn fetch_ubuntu(output_dir: &Path) -> anyhow::Result<()> {
    fetch_ubuntu_disk(output_dir)?;
    fetch_ubuntu_kernel()?;
    Ok(())
}

fn fetch_ubuntu_disk(output_dir: &Path) -> anyhow::Result<()> {
    let dest = output_dir.join(UBUNTU_RAW_NAME);

    if dest.exists() {
        println!(
            "[fetch-guest] Ubuntu {} image already present: {}",
            UBUNTU_VERSION,
            dest.display()
        );
        return Ok(());
    }

    let url = format!("{}/{}", UBUNTU_BASE_URL, UBUNTU_CLOUD_IMG);
    let cloud_dest = output_dir.join(UBUNTU_CLOUD_IMG);

    println!(
        "[fetch-guest] Downloading Ubuntu {} LTS AArch64 cloud image...",
        UBUNTU_VERSION
    );
    download_file(&url, &cloud_dest, 5).context("failed to download Ubuntu cloud image")?;

    println!("[fetch-guest] Converting qcow2 -> raw disk image...");
    let status = std::process::Command::new("qemu-img")
        .args([
            "convert",
            "-f",
            "qcow2",
            "-O",
            "raw",
            cloud_dest.to_str().unwrap(),
            dest.to_str().unwrap(),
        ])
        .status()
        .context("failed to run qemu-img (install qemu-utils / brew install qemu)")?;

    let _ = std::fs::remove_file(&cloud_dest);
    anyhow::ensure!(status.success(), "qemu-img convert failed");

    println!("[fetch-guest] Ubuntu raw image ready: {}", dest.display());
    Ok(())
}

/*
 * fetch_ubuntu_kernel — download the Ubuntu Noble arm64 generic kernel .deb
 * and extract the kernel binary into guest-images/ubuntu-kernel-6.8.0-Image.
 *
 * This avoids ext4 filesystem mounting entirely:
 *   1. Query the Ubuntu Noble arm64 Packages.gz index for linux-image-6.8.0-*-generic.
 *   2. Download the matching .deb (an `ar` archive).
 *   3. Extract data.tar.* from the .deb using the system `ar` command.
 *   4. Extract boot/vmlinuz-* from the data tarball using `tar`.
 *   5. Copy the resulting binary to UBUNTU_KERNEL_RELPATH.
 *
 * Requires: `ar` and `tar` (both available on macOS and all Linux distributions).
 */
fn fetch_ubuntu_kernel() -> anyhow::Result<()> {
    let root = repo_root()?;
    let kernel_dest = root.join(UBUNTU_KERNEL_RELPATH);

    if kernel_dest.exists() {
        println!(
            "[fetch-guest] Ubuntu kernel already present: {}",
            kernel_dest.display()
        );
        return Ok(());
    }

    println!("[fetch-guest] Fetching Ubuntu Noble arm64 kernel package index...");
    let pkg_index_url = format!("{}/{}", UBUNTU_PORTS_BASE, UBUNTU_PKGS_INDEX);

    let client = reqwest::blocking::Client::builder()
        .redirect(reqwest::redirect::Policy::limited(5))
        .build()
        .context("failed to build HTTP client")?;

    let resp = client
        .get(&pkg_index_url)
        .send()
        .context("failed to fetch Ubuntu Packages.gz")?;
    anyhow::ensure!(
        resp.status().is_success(),
        "HTTP {} for {}",
        resp.status(),
        pkg_index_url
    );

    let gz_bytes = resp.bytes().context("failed to read Packages.gz body")?;

    /* Decompress the gzip-encoded index */
    let mut decoder = flate2::read::GzDecoder::new(&gz_bytes[..]);
    let mut index = String::new();
    std::io::Read::read_to_string(&mut decoder, &mut index)
        .context("failed to decompress Packages.gz")?;

    /* Find the Filename: line for linux-image-6.8.0-*-generic */
    let deb_path = parse_kernel_deb_path(&index)
        .context("could not find linux-image-6.8.0-*-generic in Ubuntu package index")?;

    let deb_url = format!("{}/{}", UBUNTU_PORTS_BASE, deb_path);
    println!("[fetch-guest] Downloading kernel package: {}", deb_url);

    let tmp_dir = tempfile::TempDir::new().context("failed to create temp dir")?;
    let deb_file = tmp_dir.path().join("linux-image.deb");

    download_file(&deb_url, &deb_file, 5).context("failed to download Ubuntu kernel .deb")?;

    println!("[fetch-guest] Extracting kernel binary from .deb...");
    extract_kernel_from_deb(&deb_file, &kernel_dest, tmp_dir.path())
        .context("failed to extract kernel from .deb")?;

    println!(
        "[fetch-guest] Ubuntu kernel ready: {}",
        kernel_dest.display()
    );
    Ok(())
}

/*
 * parse_kernel_deb_path — find "Filename: pool/.../linux-image-6.8.0-*-generic*.deb"
 * in the Ubuntu Packages index.  Returns the relative path (to UBUNTU_PORTS_BASE).
 */
fn parse_kernel_deb_path(index: &str) -> Option<String> {
    let mut in_pkg = false;
    let mut filename: Option<&str> = None;

    for line in index.lines() {
        if line.starts_with("Package: linux-image-6.8.0-") && line.ends_with("-generic") {
            in_pkg = true;
            filename = None;
        } else if line.starts_with("Package: ") {
            if in_pkg && filename.is_some() {
                break;
            }
            in_pkg = false;
        }

        if in_pkg {
            if let Some(rest) = line.strip_prefix("Filename: ") {
                filename = Some(rest);
            }
        }
    }

    filename.map(|s| s.to_string())
}

/*
 * extract_kernel_from_deb — unpack kernel Image binary from a Debian .deb file.
 *
 * A .deb is an `ar` archive containing:
 *   debian-binary
 *   control.tar.{xz,gz,zst}
 *   data.tar.{xz,gz,zst}
 *
 * We use the system `ar` to list and extract `data.tar.*`, then `tar` to find
 * and extract `./boot/vmlinuz-*` from the data archive.
 */
fn extract_kernel_from_deb(
    deb_path: &Path,
    kernel_dest: &Path,
    work_dir: &Path,
) -> anyhow::Result<()> {
    /* Step 1: use `ar t` to find the data.tar.* member name */
    let ar_list = std::process::Command::new("ar")
        .arg("t")
        .arg(deb_path)
        .output()
        .context("failed to run `ar t` on .deb (is binutils/ar installed?)")?;
    anyhow::ensure!(ar_list.status.success(), "`ar t` failed on .deb");

    let members = String::from_utf8_lossy(&ar_list.stdout);
    let data_member = members
        .lines()
        .find(|l| l.starts_with("data.tar"))
        .map(|s| s.to_string())
        .context("no data.tar.* member found in .deb")?;

    /* Step 2: extract data.tar.* to work_dir */
    let status = std::process::Command::new("ar")
        .args(["x", deb_path.to_str().unwrap(), &data_member])
        .current_dir(work_dir)
        .status()
        .context("failed to run `ar x`")?;
    anyhow::ensure!(status.success(), "`ar x` failed");

    let data_tar = work_dir.join(&data_member);

    /* Step 3: list data.tar.* to find the vmlinuz path */
    let tar_list = std::process::Command::new("tar")
        .args(["tf", data_tar.to_str().unwrap()])
        .output()
        .context("failed to run `tar tf`")?;
    anyhow::ensure!(tar_list.status.success(), "`tar tf` failed");

    let tar_entries = String::from_utf8_lossy(&tar_list.stdout);
    let vmlinuz_entry = tar_entries
        .lines()
        .find(|l| {
            let base = l.trim_start_matches("./");
            base.starts_with("boot/vmlinuz-") && !base.contains(".efi")
        })
        .map(|s| s.to_string())
        .context("no boot/vmlinuz-* found in data.tar.*")?;

    /* Step 4: extract just the vmlinuz entry to work_dir */
    let status = std::process::Command::new("tar")
        .args([
            "xf",
            data_tar.to_str().unwrap(),
            "--strip-components=2",
            "-C",
            work_dir.to_str().unwrap(),
            &vmlinuz_entry,
        ])
        .status()
        .context("failed to run `tar xf`")?;
    anyhow::ensure!(status.success(), "`tar xf` of vmlinuz failed");

    /* The extracted file basename (e.g. vmlinuz-6.8.0-31-generic) */
    let vmlinuz_name = std::path::Path::new(vmlinuz_entry.trim_start_matches("./"))
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("vmlinuz");
    let extracted = work_dir.join(vmlinuz_name);

    /* Step 5: the Ubuntu arm64 vmlinuz is a gzip-compressed Image.
     * libvmm needs the raw Image binary (not the gzip wrapper).
     * Detect by checking the first two bytes for the gzip magic 0x1f8b. */
    let kernel_bytes = std::fs::read(&extracted).context("failed to read extracted vmlinuz")?;

    if kernel_bytes.starts_with(&[0x1f, 0x8b]) {
        /* Gzip-compressed — decompress to get the raw Image */
        let mut gz = flate2::read::GzDecoder::new(&kernel_bytes[..]);
        let mut raw = Vec::new();
        std::io::Read::read_to_end(&mut gz, &mut raw)
            .context("failed to decompress vmlinuz gzip")?;
        std::fs::write(kernel_dest, &raw).context("failed to write decompressed kernel")?;
        println!(
            "[fetch-guest] Decompressed gzip vmlinuz → {} ({} bytes)",
            kernel_dest.display(),
            raw.len()
        );
    } else {
        /* Already a raw Image */
        std::fs::copy(&extracted, kernel_dest).context("failed to copy kernel binary")?;
        println!(
            "[fetch-guest] Copied raw kernel → {} ({} bytes)",
            kernel_dest.display(),
            kernel_bytes.len()
        );
    }

    Ok(())
}

fn fetch_freebsd(output_dir: &Path) -> anyhow::Result<()> {
    let dest = output_dir.join(FREEBSD_RAW_NAME);

    if dest.exists() {
        println!(
            "[fetch-guest] FreeBSD {} image already present: {}",
            FREEBSD_VERSION,
            dest.display()
        );
        return Ok(());
    }

    let url = format!("{}/{}", FREEBSD_BASE_URL, FREEBSD_IMAGE_XZ);
    let xz_dest = output_dir.join(FREEBSD_IMAGE_XZ);

    println!(
        "[fetch-guest] Downloading FreeBSD {} AArch64...",
        FREEBSD_VERSION
    );
    download_file(&url, &xz_dest, 5).context("failed to download FreeBSD image")?;

    println!("[fetch-guest] Decompressing .xz...");
    let status = std::process::Command::new("xz")
        .args(["-d", "-k", xz_dest.to_str().unwrap()])
        .status()
        .context("failed to run xz (install xz-utils)")?;
    anyhow::ensure!(status.success(), "xz decompression failed");

    let qcow2_name = FREEBSD_IMAGE_XZ.trim_end_matches(".xz");
    let qcow2_dest = output_dir.join(qcow2_name);

    println!("[fetch-guest] Converting qcow2 -> raw...");
    let status = std::process::Command::new("qemu-img")
        .args([
            "convert",
            "-f",
            "qcow2",
            "-O",
            "raw",
            qcow2_dest.to_str().unwrap(),
            dest.to_str().unwrap(),
        ])
        .status()
        .context("failed to run qemu-img")?;

    let _ = std::fs::remove_file(&xz_dest);
    let _ = std::fs::remove_file(&qcow2_dest);
    anyhow::ensure!(status.success(), "qemu-img convert failed");

    println!("[fetch-guest] FreeBSD raw image ready: {}", dest.display());
    Ok(())
}

fn download_file(url: &str, dest: &Path, max_redirects: u8) -> anyhow::Result<()> {
    let client = reqwest::blocking::Client::builder()
        .redirect(reqwest::redirect::Policy::limited(max_redirects as usize))
        .build()
        .context("failed to build HTTP client")?;

    let mut response = client
        .get(url)
        .send()
        .with_context(|| format!("HTTP GET failed: {}", url))?;

    anyhow::ensure!(
        response.status().is_success(),
        "HTTP {} for {}",
        response.status(),
        url
    );

    let total_bytes = response.content_length();

    let pb = ProgressBar::new(total_bytes.unwrap_or(0));
    pb.set_style(
        ProgressStyle::with_template(
            "{spinner:.green} [{elapsed_precise}] [{wide_bar:.cyan/blue}] {bytes}/{total_bytes} ({eta})",
        )
        .unwrap_or_else(|_| ProgressStyle::default_bar())
        .progress_chars("#>-"),
    );
    if total_bytes.is_none() {
        pb.set_style(
            ProgressStyle::with_template("{spinner:.green} [{elapsed_precise}] {bytes} downloaded")
                .unwrap_or_else(|_| ProgressStyle::default_bar()),
        );
    }

    let mut file = std::fs::File::create(dest)
        .with_context(|| format!("failed to create file: {}", dest.display()))?;

    let mut downloaded: u64 = 0;
    let mut buf = vec![0u8; 65536];
    loop {
        use std::io::Read;
        let n = response
            .read(&mut buf)
            .context("read error during download")?;
        if n == 0 {
            break;
        }
        file.write_all(&buf[..n])
            .context("write error during download")?;
        downloaded += n as u64;
        pb.set_position(downloaded);
    }
    pb.finish_with_message("download complete");

    if downloaded < 1_048_576 {
        let _ = std::fs::remove_file(dest);
        anyhow::bail!(
            "Download looks wrong (only {} bytes). Check URL: {}",
            downloaded,
            url
        );
    }

    Ok(())
}
