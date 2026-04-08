use anyhow::{Context, Result};
use clap::Parser;
use std::path::PathBuf;

// ── Constants ─────────────────────────────────────────────────────────── //

const MAGIC: u32 = 0x5649_4245; // "VIBE" in little-endian bytes: 0x45 0x42 0x49 0x56
const VERSION: u32 = 1;
const CODE_FORMAT_WASM: u32 = 1;
const HEADER_SIZE: usize = 56;
const CODE_OFFSET: usize = 64; // align to 64-byte boundary after 56-byte header (8 bytes padding)

// ── Core image builder ────────────────────────────────────────────────── //

/// Build a VIBE swap slot image: 56-byte header + padding to CODE_OFFSET + WASM payload.
///
/// Header layout (all little-endian):
///   0x00  magic:        u32  = 0x56494245
///   0x04  version:      u32  = 1
///   0x08  code_format:  u32  = 1 (WASM)
///   0x0C  code_offset:  u32  = 64
///   0x10  code_size:    u32  = wasm.len()
///   0x14  service_id:   u32
///   0x18  service_name: [u8; 32]  (null-padded UTF-8)
///   total: 4+4+4+4+4+4+32 = 56 bytes
pub fn build_image(wasm: &[u8], service_name: &str, service_id: u32) -> Vec<u8> {
    let mut header = Vec::with_capacity(HEADER_SIZE);

    header.extend_from_slice(&MAGIC.to_le_bytes());
    header.extend_from_slice(&VERSION.to_le_bytes());
    header.extend_from_slice(&CODE_FORMAT_WASM.to_le_bytes());
    header.extend_from_slice(&(CODE_OFFSET as u32).to_le_bytes());
    header.extend_from_slice(&(wasm.len() as u32).to_le_bytes());
    header.extend_from_slice(&service_id.to_le_bytes());

    // service_name: exactly 32 bytes, null-padded
    let mut name_field = [0u8; 32];
    let name_bytes = service_name.as_bytes();
    let copy_len = name_bytes.len().min(32);
    name_field[..copy_len].copy_from_slice(&name_bytes[..copy_len]);
    header.extend_from_slice(&name_field);

    assert_eq!(header.len(), HEADER_SIZE, "header must be exactly {} bytes", HEADER_SIZE);

    // Pad to CODE_OFFSET
    let padding = CODE_OFFSET - HEADER_SIZE;
    let mut image = header;
    image.extend(std::iter::repeat(0u8).take(padding));
    assert_eq!(image.len(), CODE_OFFSET);

    // Append WASM payload
    image.extend_from_slice(wasm);
    image
}

// ── CLI ───────────────────────────────────────────────────────────────── //

#[derive(Parser, Debug)]
#[command(name = "make-swap-image", about = "Build a VIBE swap slot image from a WASM binary")]
struct Cli {
    /// Input .wasm file
    input: PathBuf,

    /// Output image file
    output: PathBuf,

    /// Service name (max 32 bytes, null-padded)
    #[arg(long, default_value = "echo_service")]
    service_name: String,

    /// Service ID
    #[arg(long, default_value_t = 2)]
    service_id: u32,
}

fn main() -> Result<()> {
    let cli = Cli::parse();

    let wasm = std::fs::read(&cli.input)
        .with_context(|| format!("reading {}", cli.input.display()))?;

    let image = build_image(&wasm, &cli.service_name, cli.service_id);

    std::fs::write(&cli.output, &image)
        .with_context(|| format!("writing {}", cli.output.display()))?;

    println!(
        "Created {} ({} bytes: {} header + {} wasm)",
        cli.output.display(),
        image.len(),
        CODE_OFFSET,
        wasm.len()
    );

    Ok(())
}

// ── Tests ─────────────────────────────────────────────────────────────── //

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_wasm() -> Vec<u8> {
        // Minimal valid-looking WASM: magic + version
        let mut w = Vec::new();
        w.extend_from_slice(b"\0asm");
        w.extend_from_slice(&[1u8, 0, 0, 0]);
        w
    }

    #[test]
    fn test_header_magic() {
        let wasm = sample_wasm();
        let img = build_image(&wasm, "test_svc", 1);
        // First 4 bytes = MAGIC in little-endian
        let magic = u32::from_le_bytes([img[0], img[1], img[2], img[3]]);
        assert_eq!(magic, MAGIC, "magic mismatch");
    }

    #[test]
    fn test_code_offset() {
        let wasm = sample_wasm();
        let img = build_image(&wasm, "test_svc", 1);
        // Bytes at CODE_OFFSET should match the start of the WASM input
        assert!(
            img.len() >= CODE_OFFSET + wasm.len(),
            "image too short"
        );
        assert_eq!(
            &img[CODE_OFFSET..CODE_OFFSET + wasm.len()],
            wasm.as_slice(),
            "WASM payload not at CODE_OFFSET"
        );
    }

    #[test]
    fn test_service_name_padding() {
        let wasm = sample_wasm();
        let img = build_image(&wasm, "hi", 99);
        // service_name field starts at offset 0x18 = 24
        let name_field = &img[24..56];
        assert_eq!(name_field.len(), 32, "service_name field must be 32 bytes");
        assert_eq!(&name_field[..2], b"hi", "service name mismatch");
        // Remaining bytes must be zero
        assert!(
            name_field[2..].iter().all(|&b| b == 0),
            "service_name must be null-padded"
        );
    }
}
