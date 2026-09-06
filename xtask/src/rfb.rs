//! Minimal RFB verifier for the headless desktop acceptance gate.
//!
//! This is deliberately not a viewer.  It negotiates an RFB 3.8 session
//! carried through an authenticated SSH tunnel, requests one raw framebuffer
//! update, and returns compact evidence that pixel bytes were received.

use anyhow::Context;
use std::io::{Read, Write};

const RFB_3_8: &[u8; 12] = b"RFB 003.008\n";
const SECURITY_NONE: u8 = 1;
const ENCODING_RAW: i32 = 0;
const MAX_DESKTOP_DIMENSION: u16 = 16_384;
const MAX_DESKTOP_NAME: usize = 1 << 20;
const MAX_FRAME_BYTES: usize = 256 << 20;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RfbFrameEvidence {
    pub width: u16,
    pub height: u16,
    pub bytes_received: usize,
    pub fnv1a64: u64,
    pub desktop_name: String,
}

pub fn verify_raw_frame<T: Read + Write>(stream: &mut T) -> anyhow::Result<RfbFrameEvidence> {
    let mut version = [0u8; 12];
    stream
        .read_exact(&mut version)
        .context("failed to read RFB protocol version")?;
    anyhow::ensure!(
        &version == RFB_3_8,
        "desktop server offered unsupported RFB version {:?}",
        String::from_utf8_lossy(&version)
    );
    stream
        .write_all(RFB_3_8)
        .context("failed to select RFB 3.8")?;

    let security_count = read_u8(stream).context("failed to read RFB security count")?;
    anyhow::ensure!(
        security_count > 0,
        "RFB server rejected the connection before security negotiation"
    );
    let mut security_types = vec![0u8; security_count as usize];
    stream
        .read_exact(&mut security_types)
        .context("failed to read RFB security types")?;
    anyhow::ensure!(
        security_types.contains(&SECURITY_NONE),
        "RFB server does not permit tunnel-confined security type None"
    );
    stream
        .write_all(&[SECURITY_NONE])
        .context("failed to select RFB security type None")?;

    let security_result = read_u32_be(stream).context("failed to read RFB security result")?;
    anyhow::ensure!(
        security_result == 0,
        "RFB security negotiation failed with status {security_result}"
    );

    stream
        .write_all(&[1])
        .context("failed to send shared RFB ClientInit")?;

    let width = read_u16_be(stream).context("failed to read RFB width")?;
    let height = read_u16_be(stream).context("failed to read RFB height")?;
    anyhow::ensure!(
        width > 0
            && height > 0
            && width <= MAX_DESKTOP_DIMENSION
            && height <= MAX_DESKTOP_DIMENSION,
        "RFB server reported invalid framebuffer dimensions {width}x{height}"
    );

    let mut pixel_format = [0u8; 16];
    stream
        .read_exact(&mut pixel_format)
        .context("failed to read RFB pixel format")?;
    let bits_per_pixel = pixel_format[0];
    anyhow::ensure!(
        matches!(bits_per_pixel, 8 | 16 | 32),
        "unsupported RFB pixel width {bits_per_pixel}"
    );
    let bytes_per_pixel = usize::from(bits_per_pixel / 8);

    let name_len = usize::try_from(
        read_u32_be(stream).context("failed to read RFB desktop-name length")?,
    )
    .context("RFB desktop-name length does not fit usize")?;
    anyhow::ensure!(
        name_len <= MAX_DESKTOP_NAME,
        "RFB desktop name exceeds {MAX_DESKTOP_NAME} bytes"
    );
    let mut name = vec![0u8; name_len];
    stream
        .read_exact(&mut name)
        .context("failed to read RFB desktop name")?;
    let desktop_name = String::from_utf8_lossy(&name).into_owned();

    let mut set_encodings = [0u8; 8];
    set_encodings[0] = 2;
    set_encodings[2..4].copy_from_slice(&1u16.to_be_bytes());
    set_encodings[4..8].copy_from_slice(&ENCODING_RAW.to_be_bytes());
    stream
        .write_all(&set_encodings)
        .context("failed to require raw RFB encoding")?;

    let mut update_request = [0u8; 10];
    update_request[0] = 3;
    update_request[6..8].copy_from_slice(&width.to_be_bytes());
    update_request[8..10].copy_from_slice(&height.to_be_bytes());
    stream
        .write_all(&update_request)
        .context("failed to request an RFB framebuffer update")?;
    stream.flush().context("failed to flush RFB request")?;

    let message_type = read_u8(stream).context("failed to read RFB server message type")?;
    anyhow::ensure!(
        message_type == 0,
        "expected an RFB FramebufferUpdate, received message type {message_type}"
    );
    let _padding = read_u8(stream)?;
    let rectangles = read_u16_be(stream).context("failed to read RFB rectangle count")?;
    anyhow::ensure!(
        rectangles > 0,
        "RFB update contained no framebuffer rectangles"
    );

    let mut bytes_received = 0usize;
    let mut hash = 0xcbf29ce484222325u64;
    for _ in 0..rectangles {
        let _x = read_u16_be(stream)?;
        let _y = read_u16_be(stream)?;
        let rect_width = read_u16_be(stream)?;
        let rect_height = read_u16_be(stream)?;
        let encoding = read_i32_be(stream)?;
        anyhow::ensure!(
            encoding == ENCODING_RAW,
            "RFB server ignored raw-only encoding request and sent {encoding}"
        );

        let pixel_bytes = usize::from(rect_width)
            .checked_mul(usize::from(rect_height))
            .and_then(|pixels| pixels.checked_mul(bytes_per_pixel))
            .context("RFB rectangle byte count overflow")?;
        let next_total = bytes_received
            .checked_add(pixel_bytes)
            .context("RFB frame byte count overflow")?;
        anyhow::ensure!(
            next_total <= MAX_FRAME_BYTES,
            "RFB frame exceeds {MAX_FRAME_BYTES} bytes"
        );

        let mut pixels = vec![0u8; pixel_bytes];
        stream
            .read_exact(&mut pixels)
            .context("failed to read raw RFB pixels")?;
        for byte in pixels {
            hash ^= u64::from(byte);
            hash = hash.wrapping_mul(0x100000001b3);
        }
        bytes_received = next_total;
    }

    anyhow::ensure!(
        bytes_received > 0,
        "RFB update contained no raw framebuffer bytes"
    );
    Ok(RfbFrameEvidence {
        width,
        height,
        bytes_received,
        fnv1a64: hash,
        desktop_name,
    })
}

fn read_u8<T: Read>(stream: &mut T) -> anyhow::Result<u8> {
    let mut bytes = [0u8; 1];
    stream.read_exact(&mut bytes)?;
    Ok(bytes[0])
}

fn read_u16_be<T: Read>(stream: &mut T) -> anyhow::Result<u16> {
    let mut bytes = [0u8; 2];
    stream.read_exact(&mut bytes)?;
    Ok(u16::from_be_bytes(bytes))
}

fn read_u32_be<T: Read>(stream: &mut T) -> anyhow::Result<u32> {
    let mut bytes = [0u8; 4];
    stream.read_exact(&mut bytes)?;
    Ok(u32::from_be_bytes(bytes))
}

fn read_i32_be<T: Read>(stream: &mut T) -> anyhow::Result<i32> {
    let mut bytes = [0u8; 4];
    stream.read_exact(&mut bytes)?;
    Ok(i32::from_be_bytes(bytes))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Cursor, Result as IoResult};

    struct Duplex {
        input: Cursor<Vec<u8>>,
        output: Vec<u8>,
    }

    impl Duplex {
        fn new(input: Vec<u8>) -> Self {
            Self {
                input: Cursor::new(input),
                output: Vec::new(),
            }
        }
    }

    impl Read for Duplex {
        fn read(&mut self, buffer: &mut [u8]) -> IoResult<usize> {
            self.input.read(buffer)
        }
    }

    impl Write for Duplex {
        fn write(&mut self, buffer: &[u8]) -> IoResult<usize> {
            self.output.extend_from_slice(buffer);
            Ok(buffer.len())
        }

        fn flush(&mut self) -> IoResult<()> {
            Ok(())
        }
    }

    fn raw_server_conversation(security_types: &[u8], pixels: &[u8]) -> Vec<u8> {
        let mut input = Vec::new();
        input.extend_from_slice(RFB_3_8);
        input.push(security_types.len() as u8);
        input.extend_from_slice(security_types);
        if security_types.contains(&SECURITY_NONE) {
            input.extend_from_slice(&0u32.to_be_bytes());
            input.extend_from_slice(&2u16.to_be_bytes());
            input.extend_from_slice(&1u16.to_be_bytes());
            input.extend_from_slice(&[
                32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0,
            ]);
            input.extend_from_slice(&4u32.to_be_bytes());
            input.extend_from_slice(b"test");
            input.extend_from_slice(&[0, 0]);
            input.extend_from_slice(&1u16.to_be_bytes());
            input.extend_from_slice(&0u16.to_be_bytes());
            input.extend_from_slice(&0u16.to_be_bytes());
            input.extend_from_slice(&2u16.to_be_bytes());
            input.extend_from_slice(&1u16.to_be_bytes());
            input.extend_from_slice(&ENCODING_RAW.to_be_bytes());
            input.extend_from_slice(pixels);
        }
        input
    }

    #[test]
    fn verifies_one_raw_frame_and_returns_compact_evidence() {
        let pixels = [0x10, 0x20, 0x30, 0, 0x40, 0x50, 0x60, 0];
        let mut stream = Duplex::new(raw_server_conversation(&[SECURITY_NONE], &pixels));

        let evidence = verify_raw_frame(&mut stream).expect("raw RFB frame should verify");

        assert_eq!(evidence.width, 2);
        assert_eq!(evidence.height, 1);
        assert_eq!(evidence.bytes_received, pixels.len());
        assert_ne!(evidence.fnv1a64, 0);
        assert_eq!(evidence.desktop_name, "test");
        assert!(stream.output.starts_with(RFB_3_8));
        assert!(stream.output.windows(8).any(|window| window == [2, 0, 0, 1, 0, 0, 0, 0]));
    }

    #[test]
    fn rejects_a_server_that_cannot_rely_on_the_ssh_tunnel() {
        let mut stream = Duplex::new(raw_server_conversation(&[2], &[]));

        let error = verify_raw_frame(&mut stream).expect_err("VNC authentication is not the gate");

        assert!(error
            .to_string()
            .contains("tunnel-confined security type None"));
    }

    #[test]
    fn rejects_an_empty_framebuffer_update() {
        let mut input = raw_server_conversation(&[SECURITY_NONE], &[0; 8]);
        let rectangle_count_offset = input.len() - 8 - 12 - 2;
        input[rectangle_count_offset..rectangle_count_offset + 2]
            .copy_from_slice(&0u16.to_be_bytes());
        input.truncate(rectangle_count_offset + 2);
        let mut stream = Duplex::new(input);

        let error = verify_raw_frame(&mut stream).expect_err("empty update is not evidence");

        assert!(error.to_string().contains("no framebuffer rectangles"));
    }
}
