#!/usr/bin/env python3
"""
Create a swap slot image from a WASM binary.

The image has the swap_slot_header_t prefix that the controller writes
to the swap slot's shared memory region.

Usage: python3 make_swap_image.py echo_service.wasm echo_service.img

Header format (from swap_slot.c):
    uint32_t magic;          /* 0x56494245 = "VIBE" */
    uint32_t version;
    uint32_t code_format;    /* 0=idle, 1=WASM, 2=bytecode */
    uint32_t code_offset;    /* Offset from region base to code */
    uint32_t code_size;      /* Code size in bytes */
    uint32_t service_id;     /* Which service this replaces */
    char     service_name[32];
"""

import struct
import sys

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input.wasm> <output.img> [service_name] [service_id]")
        sys.exit(1)

    wasm_path = sys.argv[1]
    out_path = sys.argv[2]
    service_name = sys.argv[3] if len(sys.argv) > 3 else "echo_service"
    service_id = int(sys.argv[4]) if len(sys.argv) > 4 else 2  # SVC_TOOLSVC

    with open(wasm_path, 'rb') as f:
        wasm_data = f.read()

    # Header is 56 bytes (4+4+4+4+4+4+32)
    HEADER_SIZE = 56
    # Align code to 16-byte boundary after header
    code_offset = (HEADER_SIZE + 15) & ~15  # = 64

    # Pack header
    name_bytes = service_name.encode('utf-8')[:31] + b'\x00'
    name_bytes = name_bytes.ljust(32, b'\x00')

    header = struct.pack('<IIIIII32s',
        0x56494245,       # magic: "VIBE"
        1,                # version
        1,                # code_format: WASM
        code_offset,      # code_offset
        len(wasm_data),   # code_size
        service_id,       # service_id
        name_bytes,       # service_name
    )

    # Pad to code_offset
    padding = b'\x00' * (code_offset - len(header))

    with open(out_path, 'wb') as f:
        f.write(header)
        f.write(padding)
        f.write(wasm_data)

    print(f"Created swap image: {out_path}")
    print(f"  Header: {HEADER_SIZE} bytes")
    print(f"  Code offset: {code_offset}")
    print(f"  WASM size: {len(wasm_data)} bytes")
    print(f"  Total image: {code_offset + len(wasm_data)} bytes")
    print(f"  Service: {service_name} (id={service_id})")

if __name__ == '__main__':
    main()
