//! Managed writable copies for a two-cold-boot storage proof.
//! Never points QEMU's persistent writes at staged guest artifacts.
use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};

#[derive(Debug, Serialize, Deserialize, PartialEq)]
struct Source {
    path: PathBuf,
    bytes: u64,
    sha256: String,
}

fn identify(path: &Path) -> Result<Source> {
    let path = path.canonicalize()?;
    let mut file = File::open(&path)?;
    let bytes = file.metadata()?.len();
    ensure!(bytes > 0, "persistent proof source is empty");
    let mut hash = Sha256::new();
    let mut buffer = [0u8; 65536];
    loop {
        let count = file.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        hash.update(&buffer[..count]);
    }
    Ok(Source {
        path,
        bytes,
        sha256: format!("{:x}", hash.finalize()),
    })
}

pub(crate) fn require_same_image(first: &Path, second: &Path) -> Result<()> {
    let a = identify(first)?;
    let b = identify(second)?;
    ensure!(
        a.bytes == b.bytes && a.sha256 == b.sha256,
        "agentOS image changed between cold boots"
    );
    Ok(())
}

pub(crate) fn prepare(source: &Path, directory: &Path, second_boot: bool) -> Result<PathBuf> {
    let source = identify(source)?;
    let directory = directory.canonicalize()?;
    let disk = directory.join("guest.raw");
    let receipt = directory.join("source.json");
    ensure!(
        disk != source.path,
        "persistent proof cannot overwrite its source"
    );
    if second_boot {
        let previous: Source = serde_json::from_slice(&fs::read(&receipt)?)?;
        ensure!(
            source == previous,
            "staged guest media changed between cold boots"
        );
        let metadata = fs::symlink_metadata(&disk)?;
        ensure!(
            metadata.is_file() && metadata.len() == source.bytes,
            "persistent guest copy missing, redirected or resized"
        );
    } else {
        ensure!(
            !receipt.exists(),
            "persistent proof directory was already used"
        );
        let mut output = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&disk)
            .context("refusing to replace an existing persistent proof disk")?;
        let mut input = File::open(&source.path)?;
        ensure!(
            std::io::copy(&mut input, &mut output)? == source.bytes,
            "short media copy"
        );
        output.sync_all()?;
        let copied = identify(&disk)?;
        ensure!(
            copied.bytes == source.bytes && copied.sha256 == source.sha256,
            "persistent proof copy differs from staged guest media"
        );
        let mut record = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&receipt)?;
        record.write_all(&serde_json::to_vec_pretty(&source)?)?;
        record.sync_all()?;
    }
    Ok(disk)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn persistent_copy_retains_writes_without_modifying_base() {
        let root = tempfile::tempdir().unwrap();
        let base = root.path().join("base.raw");
        fs::write(&base, b"pristine base").unwrap();
        let work = tempfile::tempdir_in(root.path()).unwrap();
        let disk = prepare(&base, work.path(), false).unwrap();
        use std::os::unix::fs::PermissionsExt;
        assert_eq!(
            fs::metadata(&disk).unwrap().permissions().mode() & 0o777,
            0o600
        );
        require_same_image(&base, &disk).unwrap();
        fs::write(&disk, b"changed disk!").unwrap();
        assert!(require_same_image(&base, &disk).is_err());
        assert_eq!(prepare(&base, work.path(), true).unwrap(), disk);
        assert_eq!(fs::read(&disk).unwrap(), b"changed disk!");
        assert_eq!(fs::read(&base).unwrap(), b"pristine base");
        assert!(prepare(&base, work.path(), false).is_err());
        fs::write(&base, b"altered base!").unwrap();
        assert!(prepare(&base, work.path(), true).is_err());
    }
    #[test]
    fn refuses_redirected_disk_and_existing_destination() {
        let root = tempfile::tempdir().unwrap();
        let base = root.path().join("base.raw");
        fs::write(&base, b"base").unwrap();
        let work = tempfile::tempdir_in(root.path()).unwrap();
        std::os::unix::fs::symlink(&base, work.path().join("guest.raw")).unwrap();
        assert!(prepare(&base, work.path(), false).is_err());
        fs::remove_file(work.path().join("guest.raw")).unwrap();
        let disk = prepare(&base, work.path(), false).unwrap();
        fs::remove_file(&disk).unwrap();
        std::os::unix::fs::symlink(&base, &disk).unwrap();
        assert!(prepare(&base, work.path(), true).is_err());
        assert_eq!(fs::read(&base).unwrap(), b"base");
    }
}
