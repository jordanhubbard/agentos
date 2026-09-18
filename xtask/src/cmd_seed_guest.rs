//! Prepare a disposable ext4 root copy with the stock cloud-init NoCloud ABI.
use anyhow::{ensure, Context};
use std::{fs, path::PathBuf, process::Command};

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
    fs::hard_link(dir.join("root.ext4"), &args.output)
        .context("publish seeded root without replacing existing output")?;
    println!("Seeded root: {}", args.output.display());
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
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
