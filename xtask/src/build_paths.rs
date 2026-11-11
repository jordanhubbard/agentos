//! Scratch products belong to the checkout so `make clean` can remove them.
use std::io;
use std::path::PathBuf;

fn temporary_root() -> io::Result<PathBuf> {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("xtask is inside the workspace")
        .join("_build/tmp");
    std::fs::create_dir_all(&root)?;
    Ok(root)
}

pub(crate) fn tempdir() -> io::Result<tempfile::TempDir> {
    tempfile::Builder::new().tempdir_in(temporary_root()?)
}

pub(crate) fn tempfile() -> io::Result<tempfile::NamedTempFile> {
    tempfile::NamedTempFile::new_in(temporary_root()?)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scratch_products_are_checkout_local_and_removed_on_drop() {
        let root = temporary_root().unwrap();
        let directory = tempdir().unwrap();
        let file = tempfile().unwrap();
        assert_eq!(directory.path().parent(), Some(root.as_path()));
        assert_eq!(file.path().parent(), Some(root.as_path()));
        let directory_path = directory.path().to_owned();
        let file_path = file.path().to_owned();
        std::fs::write(directory.path().join("output.o"), b"object").unwrap();
        drop(directory);
        drop(file);
        assert!(!directory_path.exists());
        assert!(!file_path.exists());
    }
}
