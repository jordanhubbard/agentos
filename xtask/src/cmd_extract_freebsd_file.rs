use crate::ExtractFreebsdFileArgs;
use anyhow::{Context, Result};
use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom};

const ROOT_INO: u64 = 2;
const UFS2_MAGIC: u32 = 0x1954_0119;

#[derive(Clone)]
struct Fs {
    part_offset: u64,
    bsize: u64,
    fsize: u64,
    frag: u64,
    iblkno: u64,
    inopb: u64,
    ipg: u64,
    fpg: u64,
    nindir: u64,
}

struct Inode {
    ino: u64,
    mode: u16,
    size: u64,
    direct: [u64; 12],
    indirect: [u64; 3],
}

fn le_u16(data: &[u8], offset: usize) -> Result<u16> {
    Ok(u16::from_le_bytes(
        data.get(offset..offset + 2)
            .context("truncated u16")?
            .try_into()?,
    ))
}

fn le_u32(data: &[u8], offset: usize) -> Result<u32> {
    Ok(u32::from_le_bytes(
        data.get(offset..offset + 4)
            .context("truncated u32")?
            .try_into()?,
    ))
}

fn le_u64(data: &[u8], offset: usize) -> Result<u64> {
    Ok(u64::from_le_bytes(
        data.get(offset..offset + 8)
            .context("truncated u64")?
            .try_into()?,
    ))
}

fn read_at(file: &mut File, offset: u64, size: usize) -> Result<Vec<u8>> {
    file.seek(SeekFrom::Start(offset))?;
    let mut data = vec![0u8; size];
    file.read_exact(&mut data)
        .with_context(|| format!("short read at 0x{offset:x}"))?;
    Ok(data)
}

fn find_root_partition(file: &mut File) -> Result<u64> {
    let header = read_at(file, 512, 92)?;
    anyhow::ensure!(&header[..8] == b"EFI PART", "image has no GPT header");
    let entries_lba = le_u64(&header, 72)?;
    let entries_count = le_u32(&header, 80)? as usize;
    let entry_size = le_u32(&header, 84)? as usize;
    anyhow::ensure!(entry_size >= 128, "GPT entry size is too small");

    let mut fallback: Option<(u64, u64)> = None;
    for index in 0..entries_count {
        let offset = entries_lba
            .checked_mul(512)
            .and_then(|base| base.checked_add((index * entry_size) as u64))
            .context("GPT entry offset overflow")?;
        let entry = read_at(file, offset, entry_size)?;
        if entry[..16].iter().all(|byte| *byte == 0) {
            continue;
        }
        let first_lba = le_u64(&entry, 32)?;
        let last_lba = le_u64(&entry, 40)?;
        anyhow::ensure!(last_lba >= first_lba, "invalid GPT partition bounds");
        let units: Vec<u16> = entry[56..128]
            .chunks_exact(2)
            .map(|chunk| u16::from_le_bytes([chunk[0], chunk[1]]))
            .take_while(|unit| *unit != 0)
            .collect();
        let name = String::from_utf16_lossy(&units);
        let sectors = last_lba - first_lba + 1;
        if name == "rootfs" {
            return first_lba
                .checked_mul(512)
                .context("partition offset overflow");
        }
        if fallback.map(|item| sectors > item.0).unwrap_or(true) {
            fallback = Some((sectors, first_lba * 512));
        }
    }
    fallback
        .map(|item| item.1)
        .context("no usable GPT partition found")
}

fn open_fs(file: &mut File) -> Result<Fs> {
    let part_offset = find_root_partition(file)?;
    let superblock = read_at(file, part_offset + 65_536, 2048)?;
    anyhow::ensure!(
        le_u32(&superblock, 1372)? == UFS2_MAGIC,
        "root partition is not UFS2"
    );
    let value = |offset| -> Result<u64> { Ok(le_u32(&superblock, offset)? as u64) };
    let fs = Fs {
        part_offset,
        bsize: value(0x30)?,
        fsize: value(0x34)?,
        frag: value(0x38)?,
        iblkno: value(0x10)?,
        inopb: value(0x78)?,
        ipg: value(0xb8)?,
        fpg: value(0xbc)?,
        nindir: value(0x74)?,
    };
    anyhow::ensure!(
        fs.bsize > 0
            && fs.fsize > 0
            && fs.frag > 0
            && fs.inopb > 0
            && fs.ipg > 0
            && fs.fpg > 0
            && fs.nindir > 0,
        "invalid zero-valued UFS2 geometry"
    );
    Ok(fs)
}

fn fs_block_offset(fs: &Fs, block: u64) -> Result<u64> {
    fs.part_offset
        .checked_add(
            block
                .checked_mul(fs.fsize)
                .context("block offset overflow")?,
        )
        .context("block offset overflow")
}

fn read_inode(file: &mut File, fs: &Fs, ino: u64) -> Result<Inode> {
    let cg = ino / fs.ipg;
    let local = ino % fs.ipg;
    let block = fs.fpg * cg + fs.iblkno + (local / fs.inopb) * fs.frag;
    let offset = fs_block_offset(fs, block)? + (local % fs.inopb) * 256;
    let data = read_at(file, offset, 256)?;
    let mut direct = [0u64; 12];
    let mut indirect = [0u64; 3];
    for (index, value) in direct.iter_mut().enumerate() {
        *value = le_u64(&data, 112 + index * 8)?;
    }
    for (index, value) in indirect.iter_mut().enumerate() {
        *value = le_u64(&data, 208 + index * 8)?;
    }
    Ok(Inode {
        ino,
        mode: le_u16(&data, 0)?,
        size: le_u64(&data, 16)?,
        direct,
        indirect,
    })
}

fn file_blocks(file: &mut File, fs: &Fs, inode: &Inode) -> Result<Vec<u64>> {
    let mut blocks: Vec<u64> = inode.direct.iter().copied().filter(|b| *b != 0).collect();
    if blocks.len() as u64 * fs.bsize >= inode.size {
        return Ok(blocks);
    }
    if inode.indirect[0] != 0 {
        let data = read_at(
            file,
            fs_block_offset(fs, inode.indirect[0])?,
            fs.bsize as usize,
        )?;
        for chunk in data.chunks_exact(8).take(fs.nindir as usize) {
            let block = u64::from_le_bytes(chunk.try_into()?);
            if block != 0 {
                blocks.push(block);
            }
            if blocks.len() as u64 * fs.bsize >= inode.size {
                return Ok(blocks);
            }
        }
    }
    anyhow::bail!("inode {} uses unsupported indirect blocks", inode.ino)
}

fn read_file(file: &mut File, fs: &Fs, inode: &Inode) -> Result<Vec<u8>> {
    let mut remaining = inode.size;
    let mut out = Vec::with_capacity(remaining as usize);
    for block in file_blocks(file, fs, inode)? {
        if remaining == 0 {
            break;
        }
        let size = remaining.min(fs.bsize) as usize;
        out.extend_from_slice(&read_at(file, fs_block_offset(fs, block)?, size)?);
        remaining -= size as u64;
    }
    anyhow::ensure!(
        remaining == 0,
        "inode {} ended before declared size",
        inode.ino
    );
    Ok(out)
}

fn lookup(file: &mut File, fs: &Fs, directory: &Inode, name: &str) -> Result<u64> {
    anyhow::ensure!(
        directory.mode & 0o170000 == 0o040000,
        "inode {} is not a directory",
        directory.ino
    );
    let data = read_file(file, fs, directory)?;
    let mut offset = 0usize;
    while offset + 8 <= data.len() {
        let ino = le_u32(&data, offset)? as u64;
        let record_len = le_u16(&data, offset + 4)? as usize;
        let name_len = data[offset + 7] as usize;
        if record_len < 8 || offset + record_len > data.len() || 8 + name_len > record_len {
            break;
        }
        if ino != 0 && data[offset + 8..offset + 8 + name_len] == *name.as_bytes() {
            return Ok(ino);
        }
        offset += record_len;
    }
    anyhow::bail!("path component not found: {name}")
}

fn resolve(file: &mut File, fs: &Fs, path: &str) -> Result<Inode> {
    let mut inode = read_inode(file, fs, ROOT_INO)?;
    for component in path.split('/').filter(|part| !part.is_empty()) {
        let ino = lookup(file, fs, &inode, component)?;
        inode = read_inode(file, fs, ino)?;
    }
    Ok(inode)
}

pub fn run(args: &ExtractFreebsdFileArgs) -> Result<()> {
    let mut image = File::open(&args.image)
        .with_context(|| format!("failed to open {}", args.image.display()))?;
    let fs = open_fs(&mut image)?;
    let inode = resolve(&mut image, &fs, &args.guest_path)?;
    anyhow::ensure!(
        inode.mode & 0o170000 == 0o100000,
        "{} is not a regular file",
        args.guest_path
    );
    let contents = read_file(&mut image, &fs, &inode)?;
    if let Some(parent) = args.output.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(&args.output, contents)
        .with_context(|| format!("failed to write {}", args.output.display()))?;
    Ok(())
}
