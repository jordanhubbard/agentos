// cmd_gen_image.rs — agentOS bootable image packer
//
// Reads a system description TOML (produced by gen-caps) and the compiled PD
// ELFs, then packs them into an `agentos.img` flat-binary image that the
// agentOS root task can parse at boot.
//
// Image format is documented in docs/sel4-loader-format.md.

use std::fs;
use std::io::Write;
use std::path::PathBuf;

use anyhow::{Context, Result};
use serde::Deserialize;

// ─── CLI args ────────────────────────────────────────────────────────────────

#[derive(clap::Args)]
pub struct GenImageArgs {
    /// Target architecture (aarch64, riscv64, x86_64)
    #[arg(long)]
    pub arch: String,

    /// Path to the system description TOML (from gen-caps)
    #[arg(long)]
    pub system: PathBuf,

    /// Path to the seL4 kernel ELF
    #[arg(long)]
    pub kernel: PathBuf,

    /// Path to the root task ELF
    #[arg(long, name = "root-task")]
    pub root_task: PathBuf,

    /// Directory containing <pd_name>.elf files for each PD in the system TOML
    #[arg(long, name = "pd-dir")]
    pub pd_dir: PathBuf,

    /// Output image path (default: agentos.img)
    #[arg(long, default_value = "agentos.img")]
    pub out: PathBuf,
}

// ─── System description TOML types ──────────────────────────────────────────
//
// These mirror the structs that gen-caps produces.  When cmd_gen_caps.rs is
// added to the workspace, replace the duplicate definitions here with:
//   use crate::cmd_gen_caps::{SystemDesc, PdDesc};

#[derive(Deserialize, Debug)]
pub struct SystemDesc {
    #[serde(default)]
    pub pd: Vec<PdDesc>,
}

#[derive(Deserialize, Debug)]
pub struct PdDesc {
    pub name: String,
    #[serde(default)]
    pub priority: u8,
}

// ─── Image format constants ───────────────────────────────────────────────────

/// Magic number: ASCII "AGENTOS\0" as a little-endian u64.
pub const IMAGE_MAGIC: u64 = 0x4147454E544F5300;

/// Image format version.
pub const IMAGE_VERSION: u32 = 1;

/// Total size of the file header in bytes.
pub const HEADER_SIZE: usize = 64;

/// Size of each PD entry in the PD table (bytes).
pub const PD_ENTRY_SIZE: usize = 64;

// ─── run ─────────────────────────────────────────────────────────────────────

pub fn run(args: &GenImageArgs) -> Result<()> {
    // 1. Parse system TOML
    let toml_text = fs::read_to_string(&args.system)
        .with_context(|| format!("failed to read system TOML: {}", args.system.display()))?;
    let desc: SystemDesc = toml::from_str(&toml_text)
        .with_context(|| format!("failed to parse system TOML: {}", args.system.display()))?;
    let pds = &desc.pd;

    // 2. Read kernel ELF bytes
    let kernel_bytes = fs::read(&args.kernel)
        .with_context(|| format!("failed to read kernel ELF: {}", args.kernel.display()))?;

    // 3. Read root task ELF bytes
    let root_bytes = fs::read(&args.root_task)
        .with_context(|| format!("failed to read root task ELF: {}", args.root_task.display()))?;

    // 4. Read PD ELF bytes
    let mut pd_elfs: Vec<Vec<u8>> = Vec::with_capacity(pds.len());
    for pd in pds {
        let elf_path = args.pd_dir.join(format!("{}.elf", pd.name));
        let bytes = fs::read(&elf_path)
            .with_context(|| format!("failed to read PD ELF: {}", elf_path.display()))?;
        pd_elfs.push(bytes);
    }

    // 5. Compute byte offsets
    //
    //   [0..64)                  header
    //   [64 .. 64 + N*64)        PD entry table  (N = pds.len())
    //   [pd_table_end ..)        kernel ELF
    //   [kernel_end ..)          root task ELF
    //   [root_end ..)            PD ELFs, in order

    let num_pds = pds.len() as u32;
    let pd_table_off = HEADER_SIZE as u32;
    let pd_table_size = num_pds * PD_ENTRY_SIZE as u32;

    let kernel_off = pd_table_off + pd_table_size;
    let kernel_len = kernel_bytes.len() as u32;

    let root_off = kernel_off + kernel_len;
    let root_len = root_bytes.len() as u32;

    // PD ELF offsets
    let mut pd_elf_offsets: Vec<u32> = Vec::with_capacity(pds.len());
    let mut running_off = root_off + root_len;
    for elf in &pd_elfs {
        pd_elf_offsets.push(running_off);
        running_off += elf.len() as u32;
    }

    let total_size = running_off as usize;

    // 6. Assemble the image in memory then write atomically
    let mut img: Vec<u8> = Vec::with_capacity(total_size);

    // ── Header (64 bytes) ────────────────────────────────────────────────────
    img.extend_from_slice(&IMAGE_MAGIC.to_le_bytes()); // [0..8]   magic
    img.extend_from_slice(&IMAGE_VERSION.to_le_bytes()); // [8..12]  version
    img.extend_from_slice(&num_pds.to_le_bytes()); // [12..16] num_pds
    img.extend_from_slice(&kernel_off.to_le_bytes()); // [16..20] kernel_off
    img.extend_from_slice(&kernel_len.to_le_bytes()); // [20..24] kernel_len
    img.extend_from_slice(&root_off.to_le_bytes()); // [24..28] root_off
    img.extend_from_slice(&root_len.to_le_bytes()); // [28..32] root_len
    img.extend_from_slice(&pd_table_off.to_le_bytes()); // [32..36] pd_table_off
    img.extend_from_slice(&[0u8; 28]); // [36..64] _pad

    debug_assert_eq!(img.len(), HEADER_SIZE, "header must be exactly 64 bytes");

    // ── PD entry table (num_pds × 64 bytes) ─────────────────────────────────
    for (i, pd) in pds.iter().enumerate() {
        // name: 48 bytes, null-padded
        let mut name_buf = [0u8; 48];
        let name_bytes = pd.name.as_bytes();
        let copy_len = name_bytes.len().min(47); // leave at least one NUL
        name_buf[..copy_len].copy_from_slice(&name_bytes[..copy_len]);

        img.extend_from_slice(&name_buf); // [0..48]  name
        img.extend_from_slice(&pd_elf_offsets[i].to_le_bytes()); // [48..52] elf_off
        img.extend_from_slice(&(pd_elfs[i].len() as u32).to_le_bytes()); // [52..56] elf_len
        img.push(pd.priority); // [56]     priority
        img.extend_from_slice(&[0u8; 7]); // [57..64] _pad
    }

    // ── ELF data ─────────────────────────────────────────────────────────────
    img.extend_from_slice(&kernel_bytes);
    img.extend_from_slice(&root_bytes);
    for elf in &pd_elfs {
        img.extend_from_slice(elf);
    }

    debug_assert_eq!(img.len(), total_size, "image size mismatch");

    // 7. Write output file
    let mut out_file = fs::File::create(&args.out)
        .with_context(|| format!("failed to create output file: {}", args.out.display()))?;
    out_file
        .write_all(&img)
        .with_context(|| format!("failed to write output file: {}", args.out.display()))?;

    println!(
        "[gen-image] wrote {}: {} bytes, {} PD(s), arch={}",
        args.out.display(),
        total_size,
        num_pds,
        args.arch,
    );

    Ok(())
}

// ─── Unit tests ───────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;
    use tempfile::NamedTempFile;

    /// Minimal fake ELF: 4-byte magic + 12 bytes of zeros = 16 bytes total.
    fn fake_elf() -> Vec<u8> {
        let mut v = vec![0x7f, b'E', b'L', b'F'];
        v.extend_from_slice(&[0u8; 12]);
        v
    }

    #[test]
    fn test_gen_image_magic_and_pd_count() {
        // Write temporary input files
        let kernel_file = NamedTempFile::new().unwrap();
        std::fs::write(kernel_file.path(), fake_elf()).unwrap();

        let root_file = NamedTempFile::new().unwrap();
        std::fs::write(root_file.path(), fake_elf()).unwrap();

        // PD ELFs in a temp dir
        let pd_dir = tempfile::tempdir().unwrap();
        std::fs::write(pd_dir.path().join("serial-mux.elf"), fake_elf()).unwrap();
        std::fs::write(pd_dir.path().join("net-service.elf"), fake_elf()).unwrap();

        // Minimal system TOML with two PDs
        let toml_str = r#"
[[pd]]
name = "serial-mux"
priority = 1

[[pd]]
name = "net-service"
priority = 2
"#;
        let toml_file = NamedTempFile::new().unwrap();
        std::fs::write(toml_file.path(), toml_str).unwrap();

        // Output image
        let out_file = NamedTempFile::new().unwrap();

        let args = GenImageArgs {
            arch: "aarch64".to_string(),
            system: toml_file.path().to_path_buf(),
            kernel: kernel_file.path().to_path_buf(),
            root_task: root_file.path().to_path_buf(),
            pd_dir: pd_dir.path().to_path_buf(),
            out: out_file.path().to_path_buf(),
        };

        run(&args).expect("gen-image failed");

        // Read the output image
        let mut img_bytes = Vec::new();
        std::fs::File::open(out_file.path())
            .unwrap()
            .read_to_end(&mut img_bytes)
            .unwrap();

        // Verify magic
        let magic = u64::from_le_bytes(img_bytes[0..8].try_into().unwrap());
        assert_eq!(magic, IMAGE_MAGIC, "magic mismatch");

        // Verify version
        let version = u32::from_le_bytes(img_bytes[8..12].try_into().unwrap());
        assert_eq!(version, IMAGE_VERSION);

        // Verify PD count
        let num_pds = u32::from_le_bytes(img_bytes[12..16].try_into().unwrap());
        assert_eq!(num_pds, 2, "expected 2 PDs");

        // Verify minimum expected size: header + 2*pd_entry + kernel + root + 2*pd_elf
        let expected_min = HEADER_SIZE + 2 * PD_ENTRY_SIZE + 4 * fake_elf().len();
        assert!(
            img_bytes.len() >= expected_min,
            "image too small: {} < {}",
            img_bytes.len(),
            expected_min
        );
    }

    #[test]
    fn test_gen_image_zero_pds() {
        let kernel_file = NamedTempFile::new().unwrap();
        std::fs::write(kernel_file.path(), fake_elf()).unwrap();
        let root_file = NamedTempFile::new().unwrap();
        std::fs::write(root_file.path(), fake_elf()).unwrap();

        let pd_dir = tempfile::tempdir().unwrap();

        let toml_str = "# empty system — no PDs\n";
        let toml_file = NamedTempFile::new().unwrap();
        std::fs::write(toml_file.path(), toml_str).unwrap();

        let out_file = NamedTempFile::new().unwrap();

        let args = GenImageArgs {
            arch: "riscv64".to_string(),
            system: toml_file.path().to_path_buf(),
            kernel: kernel_file.path().to_path_buf(),
            root_task: root_file.path().to_path_buf(),
            pd_dir: pd_dir.path().to_path_buf(),
            out: out_file.path().to_path_buf(),
        };

        run(&args).expect("gen-image failed on empty system");

        let img_bytes = std::fs::read(out_file.path()).unwrap();
        let magic = u64::from_le_bytes(img_bytes[0..8].try_into().unwrap());
        assert_eq!(magic, IMAGE_MAGIC);

        let num_pds = u32::from_le_bytes(img_bytes[12..16].try_into().unwrap());
        assert_eq!(num_pds, 0);
    }
}
