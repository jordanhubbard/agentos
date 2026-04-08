use crate::{FetchGuestArgs, GuestOs};
use anyhow::Context;
use indicatif::{ProgressBar, ProgressStyle};
use std::io::Write;
use std::path::{Path, PathBuf};

const UBUNTU_VERSION: &str = "24.04";
const UBUNTU_CLOUD_IMG: &str = "noble-server-cloudimg-arm64.img";
const UBUNTU_BASE_URL: &str = "https://cloud-images.ubuntu.com/noble/current";
const UBUNTU_RAW_NAME: &str = "ubuntu-24.04-aarch64.img";

const FREEBSD_VERSION: &str = "14.4";
const FREEBSD_BASE_URL: &str =
    "https://download.freebsd.org/releases/VM-IMAGES/14.4-RELEASE/aarch64/Latest";
const FREEBSD_IMAGE_XZ: &str = "FreeBSD-14.4-RELEASE-arm64-aarch64-ufs.qcow2.xz";
const FREEBSD_RAW_NAME: &str = "freebsd-14.4-aarch64.img";

pub fn run(args: &FetchGuestArgs) -> anyhow::Result<()> {
    let output_dir = PathBuf::from(&args.output_dir);
    std::fs::create_dir_all(&output_dir)
        .with_context(|| format!("failed to create output dir: {}", output_dir.display()))?;

    match args.os {
        GuestOs::Ubuntu => fetch_ubuntu(&output_dir),
        GuestOs::Freebsd => fetch_freebsd(&output_dir),
    }
}

fn fetch_ubuntu(output_dir: &Path) -> anyhow::Result<()> {
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

    // Convert qcow2 -> raw using qemu-img
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

    println!(
        "[fetch-guest] Ubuntu raw image ready: {}",
        dest.display()
    );
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

    // After xz -d -k, the decompressed file is the name without .xz
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

    println!(
        "[fetch-guest] FreeBSD raw image ready: {}",
        dest.display()
    );
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
        let n = response.read(&mut buf).context("read error during download")?;
        if n == 0 {
            break;
        }
        file.write_all(&buf[..n]).context("write error during download")?;
        downloaded += n as u64;
        pb.set_position(downloaded);
    }
    pb.finish_with_message("download complete");

    // Sanity check: real images are hundreds of MB
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
