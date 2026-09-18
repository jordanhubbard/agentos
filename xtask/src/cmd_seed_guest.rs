//! Prepare a disposable ext4 root copy with the stock cloud-init NoCloud ABI.
use anyhow::{ensure, Context};
use std::{
    fs,
    io::{Read, Seek, SeekFrom, Write},
    path::{Path, PathBuf},
    process::Command,
};

#[derive(clap::Args)]
pub struct SeedGuestArgs {
    #[arg(long)]
    pub root_ext4: PathBuf,
    #[arg(long)]
    pub public_key: PathBuf,
    #[arg(long)]
    pub output: PathBuf,
    #[arg(long)]
    pub instance_id: String,
    /// Optional source raw disk; output becomes a full disk instead of ext4.
    #[arg(long, requires = "partition_offset")]
    pub disk_raw: Option<PathBuf>,
    /// Root partition byte offset. Original root bytes must match the disk here.
    #[arg(long, requires = "disk_raw")]
    pub partition_offset: Option<u64>,
}

fn compare_region(disk: &mut fs::File, root: &Path, offset: u64) -> anyhow::Result<()> {
    disk.seek(SeekFrom::Start(offset))?;
    let mut input = fs::File::open(root)?;
    let mut expected = vec![0; 1024 * 1024];
    let mut actual = vec![0; expected.len()];
    loop {
        let count = input.read(&mut expected)?;
        if count == 0 {
            break;
        }
        disk.read_exact(&mut actual[..count])?;
        ensure!(
            actual[..count] == expected[..count],
            "disk root region differs from expected partition bytes"
        );
    }
    Ok(())
}

fn assemble_disk(
    source: &Path,
    original_root: &Path,
    seeded_root: &Path,
    output: &Path,
    offset: u64,
) -> anyhow::Result<()> {
    ensure!(source.is_file(), "disk-raw must be a regular image file");
    let length = fs::metadata(original_root)?.len();
    ensure!(
        length > 0 && length == fs::metadata(seeded_root)?.len(),
        "root partition size changed"
    );
    ensure!(
        offset % 512 == 0
            && offset
                .checked_add(length)
                .is_some_and(|end| end <= fs::metadata(source).map(|m| m.len()).unwrap_or(0)),
        "root region exceeds disk bounds or is unaligned"
    );
    fs::copy(source, output)?;
    let mut disk = fs::OpenOptions::new().read(true).write(true).open(output)?;
    // Verify the whole original partition, not merely its filesystem signature.
    compare_region(&mut disk, original_root, offset)?;
    disk.seek(SeekFrom::Start(offset))?;
    let mut root = fs::File::open(seeded_root)?;
    ensure!(
        std::io::copy(&mut root, &mut disk)? == length,
        "short root copy"
    );
    disk.flush()?;
    disk.sync_all()?;
    compare_region(&mut disk, seeded_root, offset)?;
    Ok(())
}

fn seed_files(key: &str, instance: &str) -> anyhow::Result<[(&'static str, String); 3]> {
    ensure!(
        !instance.is_empty()
            && instance.len() <= 64
            && instance
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'-'),
        "instance-id must contain 1..64 ASCII letters, digits or hyphens"
    );
    let fields: Vec<_> = key.split_whitespace().collect();
    ensure!(
        fields.len() >= 2 && fields[0] == "ssh-ed25519",
        "expected an Ed25519 public key"
    );
    ensure!(
        key.trim().lines().count() == 1
            && fields[1].len() <= 128
            && fields[1]
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"+/=".contains(&b)),
        "invalid public key encoding"
    );
    Ok([
        ("user-data", format!("#cloud-config\nusers:\n  - default\nssh_authorized_keys:\n  - ssh-ed25519 {}\nssh_pwauth: false\ndisable_root: true\nssh_deletekeys: true\nssh_genkeytypes: [ed25519]\n", fields[1])),
        ("meta-data", format!("instance-id: {instance}\nlocal-hostname: agentos-debian\n")),
        ("network-config", "version: 2\nethernets:\n  eth0:\n    dhcp4: false\n    addresses: [10.0.2.15/24]\n    routes:\n      - to: default\n        via: 10.0.2.2\n    nameservers:\n      addresses: [10.0.2.3]\n".into()),
    ])
}

pub fn run(args: &SeedGuestArgs) -> anyhow::Result<()> {
    ensure!(
        args.root_ext4.is_file(),
        "root-ext4 must be a regular image file"
    );
    ensure!(!args.output.exists(), "output already exists");
    ensure!(
        fs::metadata(&args.public_key)?.len() <= 4096,
        "public key file too large"
    );
    let files = seed_files(&fs::read_to_string(&args.public_key)?, &args.instance_id)?;
    let parent = args
        .output
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(std::path::Path::new("."));
    let staging = tempfile::Builder::new()
        .prefix(".agentos-seed-")
        .tempdir_in(parent)?;
    let dir = staging.path();
    // Fixed relative filenames prevent debugfs command parsing from interpreting paths.
    for (name, data) in &files {
        fs::write(dir.join(name), data)?;
    }
    let key = fs::read_to_string(&args.public_key)?;
    fs::write(dir.join("public-key"), key)?;
    let check = Command::new("ssh-keygen")
        .args(["-l", "-f", "public-key"])
        .current_dir(dir)
        .output()?;
    ensure!(check.status.success(), "ssh-keygen rejected the public key");
    fs::copy(&args.root_ext4, dir.join("root.ext4"))?;
    let mut script = String::from(
        "mkdir /var/lib/cloud\nmkdir /var/lib/cloud/seed\nmkdir /var/lib/cloud/seed/nocloud\n",
    );
    for (name, _) in &files {
        script.push_str(&format!(
            "write {name} /var/lib/cloud/seed/nocloud/{name}\n"
        ));
    }
    fs::write(dir.join("commands"), script)?;
    let result = Command::new("debugfs")
        .args(["-w", "-f", "commands", "root.ext4"])
        .current_dir(dir)
        .output()
        .context("run debugfs to seed disposable root")?;
    ensure!(result.status.success(), "debugfs seed failed");
    // debugfs can exit successfully despite command errors. Require exact readback.
    for (name, expected) in &files {
        let read = Command::new("debugfs")
            .args([
                "-R",
                &format!("cat /var/lib/cloud/seed/nocloud/{name}"),
                "root.ext4",
            ])
            .current_dir(dir)
            .output()?;
        ensure!(
            read.status.success() && read.stdout == expected.as_bytes(),
            "NoCloud {name} readback mismatch"
        );
    }
    fs::OpenOptions::new()
        .write(true)
        .open(dir.join("root.ext4"))?
        .sync_all()?;
    // Atomic creation without replacing an existing destination, even in a race.
    let prepared = if let Some(source) = &args.disk_raw {
        let disk = dir.join("disk.raw");
        assemble_disk(
            source,
            &args.root_ext4,
            &dir.join("root.ext4"),
            &disk,
            args.partition_offset
                .context("disk requires partition offset")?,
        )?;
        disk
    } else {
        ensure!(
            args.partition_offset.is_none(),
            "partition offset requires a disk"
        );
        dir.join("root.ext4")
    };
    fs::hard_link(prepared, &args.output)
        .context("publish seeded root without replacing existing output")?;
    println!("Seeded root: {}", args.output.display());
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn disk_assembly_preserves_surroundings_and_rejects_wrong_source() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("source");
        let original = dir.path().join("original");
        let seeded = dir.path().join("seeded");
        let output = dir.path().join("output");
        fs::write(&source, [vec![1; 512], vec![2; 512], vec![3; 512]].concat()).unwrap();
        fs::write(&original, vec![2; 512]).unwrap();
        fs::write(&seeded, vec![4; 512]).unwrap();
        assemble_disk(&source, &original, &seeded, &output, 512).unwrap();
        assert_eq!(
            fs::read(&output).unwrap(),
            [vec![1; 512], vec![4; 512], vec![3; 512]].concat()
        );
        assert_eq!(fs::read(&source).unwrap()[512], 2);
        assert!(assemble_disk(&source, &original, &seeded, &output, 0).is_err());
        assert!(assemble_disk(&source, &original, &seeded, &output, 1536).is_err());
        assert!(assemble_disk(&source, &original, &seeded, &output, 513).is_err());
    }
    #[test]
    fn seed_rejects_yaml_and_multiline_injection() {
        assert!(seed_files("ssh-ed25519 AAAA\nssh-ed25519 BBBB", "test").is_err());
        assert!(seed_files("ssh-ed25519 AAAA", "test\nusers: []").is_err());
        assert!(seed_files("ssh-rsa AAAA", "test").is_err());
        let files = seed_files("ssh-ed25519 AAAA ignored-comment", "test-1").unwrap();
        assert!(files[0].1.contains("ssh_pwauth: false\n"));
        assert!(!files[0].1.contains("ignored-comment"));
    }
}
