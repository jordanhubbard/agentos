#!/usr/bin/env python3
"""Extract a file from the UFS2 root partition of a FreeBSD VM image."""

from __future__ import annotations

import argparse
import os
import struct
import sys
from dataclasses import dataclass


ROOT_INO = 2
UFS2_MAGIC = 0x19540119


@dataclass(frozen=True)
class Fs:
    image: str
    part_offset: int
    bsize: int
    fsize: int
    frag: int
    iblkno: int
    inopb: int
    ipg: int
    fpg: int
    nindir: int


@dataclass(frozen=True)
class Inode:
    ino: int
    mode: int
    size: int
    db: tuple[int, ...]
    ib: tuple[int, ...]


def read_at(fp, off: int, size: int) -> bytes:
    fp.seek(off)
    data = fp.read(size)
    if len(data) != size:
        raise EOFError(f"short read at 0x{off:x}: wanted {size}, got {len(data)}")
    return data


def find_root_partition(fp) -> int:
    hdr = read_at(fp, 512, 92)
    if hdr[:8] != b"EFI PART":
        raise RuntimeError("image does not contain a GPT header")

    entries_lba, entries_count, entry_size = struct.unpack_from("<QII", hdr, 72)
    fp.seek(entries_lba * 512)

    fallback = None
    for _ in range(entries_count):
        entry = fp.read(entry_size)
        if len(entry) != entry_size or entry[:16] == b"\0" * 16:
            continue
        first_lba, last_lba = struct.unpack_from("<QQ", entry, 32)
        name = entry[56:128].decode("utf-16le", "ignore").rstrip("\0")
        size = last_lba - first_lba + 1
        if name == "rootfs":
            return first_lba * 512
        if fallback is None or size > fallback[0]:
            fallback = (size, first_lba * 512)

    if fallback is None:
        raise RuntimeError("no usable GPT partition found")
    return fallback[1]


def open_fs(fp, image: str) -> Fs:
    part_offset = find_root_partition(fp)
    sb = read_at(fp, part_offset + 65536, 2048)
    magic = struct.unpack_from("<I", sb, 1372)[0]
    if magic != UFS2_MAGIC:
        raise RuntimeError(f"root partition is not UFS2: magic=0x{magic:08x}")

    return Fs(
        image=image,
        part_offset=part_offset,
        bsize=struct.unpack_from("<i", sb, 0x30)[0],
        fsize=struct.unpack_from("<i", sb, 0x34)[0],
        frag=struct.unpack_from("<i", sb, 0x38)[0],
        iblkno=struct.unpack_from("<i", sb, 0x10)[0],
        inopb=struct.unpack_from("<i", sb, 0x78)[0],
        ipg=struct.unpack_from("<I", sb, 0xB8)[0],
        fpg=struct.unpack_from("<i", sb, 0xBC)[0],
        nindir=struct.unpack_from("<i", sb, 0x74)[0],
    )


def fs_block_offset(fs: Fs, fs_block: int) -> int:
    return fs.part_offset + fs_block * fs.fsize


def read_inode(fp, fs: Fs, ino: int) -> Inode:
    cg = ino // fs.ipg
    ino_in_cg = ino % fs.ipg
    inode_fs_block = fs.fpg * cg + fs.iblkno + (ino_in_cg // fs.inopb) * fs.frag
    inode_off = fs_block_offset(fs, inode_fs_block) + (ino_in_cg % fs.inopb) * 256
    di = read_at(fp, inode_off, 256)
    return Inode(
        ino=ino,
        mode=struct.unpack_from("<H", di, 0)[0],
        size=struct.unpack_from("<Q", di, 16)[0],
        db=struct.unpack_from("<12Q", di, 112),
        ib=struct.unpack_from("<3Q", di, 208),
    )


def file_blocks(fp, fs: Fs, inode: Inode) -> list[int]:
    blocks: list[int] = [block for block in inode.db if block]
    if len(blocks) * fs.bsize >= inode.size:
        return blocks

    if inode.ib[0]:
        indirect = read_at(fp, fs_block_offset(fs, inode.ib[0]), fs.bsize)
        for off in range(0, len(indirect), 8):
            block = struct.unpack_from("<Q", indirect, off)[0]
            if block:
                blocks.append(block)
            if len(blocks) * fs.bsize >= inode.size:
                return blocks

    raise RuntimeError(
        f"{inode.ino}: file uses unsupported double-indirect blocks"
    )


def read_file(fp, fs: Fs, inode: Inode) -> bytes:
    remaining = inode.size
    chunks: list[bytes] = []
    for block in file_blocks(fp, fs, inode):
        if remaining <= 0:
            break
        size = min(fs.bsize, remaining)
        chunks.append(read_at(fp, fs_block_offset(fs, block), size))
        remaining -= size
    return b"".join(chunks)


def lookup(fp, fs: Fs, dir_inode: Inode, name: str) -> int:
    if (dir_inode.mode & 0o170000) != 0o040000:
        raise RuntimeError(f"inode {dir_inode.ino} is not a directory")
    data = read_file(fp, fs, dir_inode)
    off = 0
    while off + 8 <= len(data):
        ino, reclen, dtype, namlen = struct.unpack_from("<IHBb", data, off)
        if reclen < 8:
            break
        entry_name = data[off + 8 : off + 8 + namlen].decode("utf-8", "ignore")
        if ino != 0 and entry_name == name:
            return ino
        off += reclen
    raise FileNotFoundError(name)


def resolve_path(fp, fs: Fs, path: str) -> Inode:
    inode = read_inode(fp, fs, ROOT_INO)
    for component in [p for p in path.split("/") if p]:
        inode = read_inode(fp, fs, lookup(fp, fs, inode, component))
    return inode


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("path")
    parser.add_argument("output")
    args = parser.parse_args()

    with open(args.image, "rb") as fp:
        fs = open_fs(fp, args.image)
        inode = resolve_path(fp, fs, args.path)
        if (inode.mode & 0o170000) != 0o100000:
            raise RuntimeError(f"{args.path} is not a regular file")
        data = read_file(fp, fs, inode)

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as out:
        out.write(data)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"extract_freebsd_file.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
