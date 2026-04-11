#!/usr/bin/env python3
"""
gen_policy.py — generate cap_policy.bin from a policy text file.

Usage: python3 gen_policy.py policy.txt > cap_policy.bin

Policy file format:
  agent=0 class=0x01 rights=0x07 flags=0x01 resource=0
  agent=1 class=0x03 rights=0x03 flags=0x00 resource=0
"""
import struct, sys

CAP_POLICY_MAGIC = 0x43415050
CAP_POLICY_VERSION = 1

grants = []
for line in sys.stdin if len(sys.argv) < 2 else open(sys.argv[1]):
    line = line.strip()
    if not line or line.startswith('#'): continue
    fields = dict(f.split('=') for f in line.split())
    grants.append((int(fields['agent']), int(fields.get('class','0'),16),
                   int(fields.get('rights','7'),16), int(fields.get('flags','0'),16),
                   int(fields.get('resource','0'))))

header = struct.pack('<IIII', CAP_POLICY_MAGIC, CAP_POLICY_VERSION, len(grants), 0)
body = b''.join(struct.pack('<BBBBI', *g) for g in grants)
sys.stdout.buffer.write(header + body)
