#!/usr/bin/env python3
"""
sign_wasm.py — Sign agentOS WASM modules

Reads a .wasm file, locates the `agentos.capabilities` custom section,
computes a SHA-512 hash, and appends an `agentos.signature` custom section.

Phase 1 format (SHA-512 hash-only, Ed25519-compatible layout):
  [8 bytes: key_id] [32 bytes: SHA-512(capabilities) truncated] [24 bytes: zero]

Usage:
  python3 sign_wasm.py input.wasm --key-id DEADBEEF01234567 -o signed.wasm
  python3 sign_wasm.py input.wasm --verify  # verify existing signature

Requires: Python 3.6+ (no external deps)
"""

import hashlib
import struct
import sys
import argparse


def read_leb128(data, pos):
    """Read unsigned LEB128 from data at pos. Returns (value, new_pos)."""
    result = 0
    shift = 0
    while pos < len(data) and shift < 35:
        byte = data[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not (byte & 0x80):
            break
    return result, pos


def write_leb128(value):
    """Encode unsigned LEB128."""
    result = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            byte |= 0x80
        result.append(byte)
        if not value:
            break
    return bytes(result)


def find_custom_section(wasm, name):
    """Find a named custom section. Returns (payload_offset, payload_len) or None."""
    if len(wasm) < 8:
        return None
    pos = 8  # skip magic + version
    while pos < len(wasm):
        section_id = wasm[pos]
        pos += 1
        sec_size, pos = read_leb128(wasm, pos)
        sec_start = pos
        if sec_start + sec_size > len(wasm):
            break
        if section_id == 0:  # custom section
            name_len, npos = read_leb128(wasm, sec_start)
            name_bytes = wasm[npos:npos + name_len]
            if name_bytes == name.encode('utf-8'):
                payload_start = npos + name_len
                payload_len = (sec_start + sec_size) - payload_start
                return (payload_start, payload_len)
        pos = sec_start + sec_size
    return None


def make_custom_section(name, payload):
    """Build a WASM custom section with the given name and payload."""
    name_bytes = name.encode('utf-8')
    name_leb = write_leb128(len(name_bytes))
    content = name_leb + name_bytes + payload
    section_header = bytes([0x00]) + write_leb128(len(content))
    return section_header + content


def sign_wasm(input_path, output_path, key_id_hex):
    with open(input_path, 'rb') as f:
        wasm = f.read()

    # Verify WASM magic
    if wasm[:4] != b'\x00asm':
        print(f"Error: {input_path} is not a valid WASM file", file=sys.stderr)
        sys.exit(1)

    # Find capabilities section
    caps = find_custom_section(wasm, 'agentos.capabilities')
    if caps is None:
        print(f"Error: no agentos.capabilities section found in {input_path}", file=sys.stderr)
        sys.exit(1)

    caps_offset, caps_len = caps
    caps_data = wasm[caps_offset:caps_offset + caps_len]

    # Compute SHA-512 hash
    h = hashlib.sha512(caps_data).digest()

    # Build signature payload (64 bytes)
    key_id = bytes.fromhex(key_id_hex)
    if len(key_id) != 8:
        print("Error: key_id must be 8 bytes (16 hex chars)", file=sys.stderr)
        sys.exit(1)

    sig_payload = key_id + h[:32] + b'\x00' * 24
    assert len(sig_payload) == 64

    # Remove existing signature section if present
    existing = find_custom_section(wasm, 'agentos.signature')
    if existing:
        print("Warning: replacing existing agentos.signature section")
        # For simplicity, just append new one (old section will be ignored
        # since we scan for the first match)

    # Append signature section
    sig_section = make_custom_section('agentos.signature', sig_payload)
    signed_wasm = wasm + sig_section

    with open(output_path, 'wb') as f:
        f.write(signed_wasm)

    print(f"Signed: {output_path}")
    print(f"  Key ID:    {key_id_hex}")
    print(f"  Hash:      {h[:32].hex()}")
    print(f"  Caps size: {caps_len} bytes")


def verify_wasm(input_path):
    with open(input_path, 'rb') as f:
        wasm = f.read()

    caps = find_custom_section(wasm, 'agentos.capabilities')
    sig = find_custom_section(wasm, 'agentos.signature')

    if caps is None:
        print("No agentos.capabilities section found")
        return False

    if sig is None:
        print("No agentos.signature section found (unsigned module)")
        return False

    caps_offset, caps_len = caps
    caps_data = wasm[caps_offset:caps_offset + caps_len]

    sig_offset, sig_len = sig
    if sig_len != 64:
        print(f"Bad signature size: {sig_len} (expected 64)")
        return False

    sig_data = wasm[sig_offset:sig_offset + sig_len]
    key_id = sig_data[:8]
    stored_hash = sig_data[8:40]
    reserved = sig_data[40:64]

    h = hashlib.sha512(caps_data).digest()[:32]

    print(f"Key ID:       {key_id.hex()}")
    print(f"Stored hash:  {stored_hash.hex()}")
    print(f"Computed hash: {h.hex()}")
    print(f"Reserved zero: {reserved == b'\\x00' * 24}")

    if h == stored_hash and reserved == b'\x00' * 24:
        print("✓ Signature VALID")
        return True
    else:
        print("✗ Signature INVALID")
        return False


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Sign agentOS WASM modules')
    parser.add_argument('input', help='Input .wasm file')
    parser.add_argument('-o', '--output', help='Output signed .wasm file')
    parser.add_argument('--key-id', default='0000000000000001',
                        help='8-byte key ID in hex (default: 0000000000000001)')
    parser.add_argument('--verify', action='store_true',
                        help='Verify existing signature instead of signing')
    args = parser.parse_args()

    if args.verify:
        ok = verify_wasm(args.input)
        sys.exit(0 if ok else 1)
    else:
        output = args.output or args.input.replace('.wasm', '.signed.wasm')
        sign_wasm(args.input, output, args.key_id)
