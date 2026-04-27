// cmd_gen_pd_bundle.rs — agentOS PD-bundle generator
//
// Produces a compact binary blob containing the PD entry table and all PD ELFs.
// This blob is injected into root_task.elf's `.pd_bundle` section via
// `objcopy --update-section .pd_bundle=<bundle>`.
//
// The root task reads PD ELFs from this embedded bundle at boot instead of
// walking the seL4 extra-BootInfo region (which seL4 uses only for DTB data,
// not our custom PD payloads).
//
// Bundle format — a strict subset of the agentos.img format:
//
//   [0..64)           agentos_img_hdr_t  (magic + num_pds + offsets)
//   [64..64+N*64)     agentos_pd_entry_t table  (N = num_pds)
//   [pd_elf_data..)   PD ELF blobs, concatenated
//
// kernel_off and root_off are both set to 0 (the bundle has no kernel or
// root task ELF; those are in the main agentos.img loaded by the loader).

use std::fs;
use std::io::Write;
use std::path::PathBuf;

use anyhow::{Context, Result};

// Re-use the same SystemDesc / PdDesc types from cmd_gen_image
use crate::cmd_gen_image::{SystemDesc, IMAGE_MAGIC, IMAGE_VERSION, HEADER_SIZE, PD_ENTRY_SIZE};

// ─── CLI args ────────────────────────────────────────────────────────────────

#[derive(clap::Args)]
pub struct GenPdBundleArgs {
    /// Path to the system description TOML (same one used by gen-image)
    #[arg(long)]
    pub system: PathBuf,

    /// Directory containing <pd_name>.elf files for each PD in the system TOML
    #[arg(long, name = "pd-dir")]
    pub pd_dir: PathBuf,

    /// Output bundle path
    #[arg(long)]
    pub out: PathBuf,
}

// ─── run ─────────────────────────────────────────────────────────────────────

pub fn run(args: &GenPdBundleArgs) -> Result<()> {
    // 1. Parse system TOML
    let toml_text = fs::read_to_string(&args.system)
        .with_context(|| format!("failed to read system TOML: {}", args.system.display()))?;
    let desc: SystemDesc = toml::from_str(&toml_text)
        .with_context(|| format!("failed to parse system TOML: {}", args.system.display()))?;
    let pds = &desc.pd;

    // 2. Read PD ELF bytes
    let mut pd_elfs: Vec<Vec<u8>> = Vec::with_capacity(pds.len());
    for pd in pds {
        let elf_path = args.pd_dir.join(format!("{}.elf", pd.name));
        let bytes = fs::read(&elf_path)
            .with_context(|| format!("failed to read PD ELF: {}", elf_path.display()))?;
        pd_elfs.push(bytes);
    }

    // 3. Compute offsets
    //
    //   [0..HEADER_SIZE)           agentos_img_hdr_t
    //   [HEADER_SIZE..pd_data_off) PD entry table  (N × PD_ENTRY_SIZE)
    //   [pd_data_off..)            PD ELF blobs

    let num_pds = pds.len() as u32;
    let pd_table_off = HEADER_SIZE as u32;
    let pd_table_size = num_pds * PD_ENTRY_SIZE as u32;
    let pd_data_start = pd_table_off + pd_table_size;

    // Compute each PD ELF's offset within the bundle
    let mut pd_elf_offsets: Vec<u32> = Vec::with_capacity(pds.len());
    let mut running_off = pd_data_start;
    for elf in &pd_elfs {
        pd_elf_offsets.push(running_off);
        running_off += elf.len() as u32;
    }

    let total_size = running_off as usize;

    // 4. Assemble bundle in memory
    let mut bundle: Vec<u8> = Vec::with_capacity(total_size);

    // ── Header (64 bytes) ────────────────────────────────────────────────────
    // kernel_off = 0, kernel_len = 0  (no kernel in bundle)
    // root_off   = 0, root_len   = 0  (no root_task in bundle)
    bundle.extend_from_slice(&IMAGE_MAGIC.to_le_bytes());           // [0..8]
    bundle.extend_from_slice(&IMAGE_VERSION.to_le_bytes());         // [8..12]
    bundle.extend_from_slice(&num_pds.to_le_bytes());               // [12..16]
    bundle.extend_from_slice(&0u32.to_le_bytes());                  // [16..20] kernel_off = 0
    bundle.extend_from_slice(&0u32.to_le_bytes());                  // [20..24] kernel_len = 0
    bundle.extend_from_slice(&0u32.to_le_bytes());                  // [24..28] root_off = 0
    bundle.extend_from_slice(&0u32.to_le_bytes());                  // [28..32] root_len = 0
    bundle.extend_from_slice(&pd_table_off.to_le_bytes());          // [32..36] pd_table_off
    bundle.extend_from_slice(&[0u8; 28]);                           // [36..64] _pad

    debug_assert_eq!(bundle.len(), HEADER_SIZE, "bundle header must be exactly 64 bytes");

    // ── PD entry table (num_pds × 64 bytes) ─────────────────────────────────
    for (i, pd) in pds.iter().enumerate() {
        let mut name_buf = [0u8; 48];
        let name_bytes = pd.name.as_bytes();
        let copy_len = name_bytes.len().min(47);
        name_buf[..copy_len].copy_from_slice(&name_bytes[..copy_len]);

        bundle.extend_from_slice(&name_buf);                                   // [0..48]  name
        bundle.extend_from_slice(&pd_elf_offsets[i].to_le_bytes());            // [48..52] elf_off
        bundle.extend_from_slice(&(pd_elfs[i].len() as u32).to_le_bytes());   // [52..56] elf_len
        bundle.push(pd.priority);                                              // [56]     priority
        bundle.extend_from_slice(&[0u8; 7]);                                   // [57..64] _pad
    }

    // ── PD ELF data ──────────────────────────────────────────────────────────
    for elf in &pd_elfs {
        bundle.extend_from_slice(elf);
    }

    debug_assert_eq!(bundle.len(), total_size, "bundle size mismatch");

    // 5. Write output file
    let mut out_file = fs::File::create(&args.out)
        .with_context(|| format!("failed to create bundle file: {}", args.out.display()))?;
    out_file
        .write_all(&bundle)
        .with_context(|| format!("failed to write bundle file: {}", args.out.display()))?;

    println!(
        "[gen-pd-bundle] wrote {}: {} bytes, {} PD(s)",
        args.out.display(),
        total_size,
        num_pds,
    );

    Ok(())
}

// ─── Unit tests ───────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;
    use tempfile::NamedTempFile;

    fn fake_elf() -> Vec<u8> {
        let mut v = vec![0x7f, b'E', b'L', b'F'];
        v.extend_from_slice(&[0u8; 12]);
        v
    }

    #[test]
    fn test_gen_pd_bundle_magic_and_layout() {
        let pd_dir = tempfile::tempdir().unwrap();
        std::fs::write(pd_dir.path().join("controller.elf"), fake_elf()).unwrap();
        std::fs::write(pd_dir.path().join("serial_pd.elf"), fake_elf()).unwrap();

        let toml_str = r#"
[[pd]]
name = "controller"
priority = 2

[[pd]]
name = "serial_pd"
priority = 1
"#;
        let toml_file = NamedTempFile::new().unwrap();
        std::fs::write(toml_file.path(), toml_str).unwrap();
        let out_file = NamedTempFile::new().unwrap();

        let args = GenPdBundleArgs {
            system: toml_file.path().to_path_buf(),
            pd_dir: pd_dir.path().to_path_buf(),
            out: out_file.path().to_path_buf(),
        };

        run(&args).expect("gen-pd-bundle failed");

        let mut bytes = Vec::new();
        std::fs::File::open(out_file.path())
            .unwrap()
            .read_to_end(&mut bytes)
            .unwrap();

        // Verify magic
        let magic = u64::from_le_bytes(bytes[0..8].try_into().unwrap());
        assert_eq!(magic, IMAGE_MAGIC, "magic mismatch");

        // Verify PD count
        let num_pds = u32::from_le_bytes(bytes[12..16].try_into().unwrap());
        assert_eq!(num_pds, 2);

        // kernel_off / kernel_len / root_off / root_len must all be 0
        let kernel_off = u32::from_le_bytes(bytes[16..20].try_into().unwrap());
        let kernel_len = u32::from_le_bytes(bytes[20..24].try_into().unwrap());
        let root_off   = u32::from_le_bytes(bytes[24..28].try_into().unwrap());
        let root_len   = u32::from_le_bytes(bytes[28..32].try_into().unwrap());
        assert_eq!(kernel_off, 0);
        assert_eq!(kernel_len, 0);
        assert_eq!(root_off, 0);
        assert_eq!(root_len, 0);

        // Minimum expected size: header + 2*pd_entry + 2*pd_elf
        let expected_min = HEADER_SIZE + 2 * PD_ENTRY_SIZE + 2 * fake_elf().len();
        assert!(bytes.len() >= expected_min);
    }
}
