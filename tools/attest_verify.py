#!/usr/bin/env python3
"""
attest_verify.py — Verify agentOS capability attestation reports

Fetches a capability attestation report from AgentFS (sparky:8791 or local file)
and verifies its integrity using SHA-512.

Usage:
  # Fetch latest attestation from live AgentFS:
  python3 attest_verify.py --agentfs http://sparky:8791

  # Verify a local .att file:
  python3 attest_verify.py --file attestation.att

  # Verify all reports in a directory:
  python3 attest_verify.py --dir /path/to/attestations/

  # Dump decoded contents as JSON:
  python3 attest_verify.py --file attestation.att --json

Environment:
  AGENTFS_URL — default AgentFS base URL (overrides --agentfs default)
  AGENTFS_TOKEN — auth token for AgentFS (if required)

Report format (NATT v1, little-endian):
  Offset  Size  Description
  ------  ----  -----------
    0      4    Magic: 0x4E415454 ("NATT")
    4      4    Format version (1)
    8      8    Boot tick (uint64 LE)
   16      4    active_cap_count (N)
   20      4    total_cap_slots
   24      8    audit_seq
   32    N*20   Cap entries: [cptr(4), rights(4), kind(4), owner_pd(4), granted_to(4)]
  32+N*20  8    Net ACL summary: [slots_active(4), total_denials(4)]
  40+N*20  64   SHA-512 sig: [key_id(8), sha512_truncated(32), zero_pad(24)]

Verification:
  sha512(report[0..total-64]) must match sig[8:40]
"""

import struct
import hashlib
import sys
import os
import json
import argparse
from datetime import datetime

# ─── Constants ────────────────────────────────────────────────────────────────

ATTEST_MAGIC   = 0x4E415454   # "NATT"
ATTEST_VERSION = 1
ATTEST_KEY_ID  = bytes([0x4E, 0x41, 0x54, 0x54, 0x01, 0x00, 0x00, 0x00])
CAP_ENTRY_SIZE = 20           # 5 × uint32

# Capability rights bitmasks (from agentos.h)
RIGHTS = {
    0x01: 'read',
    0x02: 'write',
    0x04: 'grant',
    0x08: 'revoke',
    0x10: 'execute',
}

# Capability kind names
CAP_KINDS = {
    0x01: 'endpoint',
    0x02: 'notification',
    0x03: 'reply',
    0x04: 'page',
    0x05: 'irq_handler',
    0x06: 'untyped',
    0x07: 'tcb',
    0x08: 'cnode',
    0x09: 'frame',
    0x0A: 'asid_pool',
    0x0B: 'asid_ctrl',
}


def decode_rights(r):
    return '|'.join(name for bit, name in sorted(RIGHTS.items()) if r & bit) or f'0x{r:02x}'


def decode_kind(k):
    return CAP_KINDS.get(k, f'kind_0x{k:02x}')


# ─── SHA-512 (stdlib) ─────────────────────────────────────────────────────────

def sha512_of(data: bytes) -> bytes:
    return hashlib.sha512(data).digest()


# ─── Report decoder ───────────────────────────────────────────────────────────

class AttestationReport:
    def __init__(self, raw: bytes, source: str = '<unknown>'):
        self.raw    = raw
        self.source = source
        self.valid  = False
        self.error  = ''
        self.magic       = 0
        self.version     = 0
        self.boot_tick   = 0
        self.active_caps = 0
        self.total_slots = 0
        self.audit_seq   = 0
        self.caps        = []
        self.net_active  = 0
        self.net_denials = 0
        self.sig_key_id  = b''
        self.sig_hash    = b''
        self.computed_hash = b''
        self._decode()

    def _decode(self):
        d = self.raw
        if len(d) < 32 + 64:
            self.error = f'Too short: {len(d)} bytes (need at least {32 + 64})'
            return

        magic, version = struct.unpack_from('<II', d, 0)
        if magic != ATTEST_MAGIC:
            self.error = f'Bad magic: 0x{magic:08X} (expected 0x{ATTEST_MAGIC:08X})'
            return
        if version != ATTEST_VERSION:
            self.error = f'Unknown version: {version} (expected {ATTEST_VERSION})'
            return

        self.magic   = magic
        self.version = version
        self.boot_tick,   = struct.unpack_from('<Q', d, 8)
        self.active_caps, = struct.unpack_from('<I', d, 16)
        self.total_slots, = struct.unpack_from('<I', d, 20)
        self.audit_seq,   = struct.unpack_from('<Q', d, 24)

        cap_data_start = 32
        expected_cap_bytes = self.active_caps * CAP_ENTRY_SIZE
        expected_total = 32 + expected_cap_bytes + 8 + 64

        if len(d) < expected_total:
            self.error = (f'Truncated: {len(d)} bytes, need {expected_total} '
                         f'for {self.active_caps} caps')
            return

        self.caps = []
        pos = cap_data_start
        for i in range(self.active_caps):
            cptr, rights, kind, owner_pd, granted_to = struct.unpack_from('<IIIII', d, pos)
            self.caps.append({
                'index':      i,
                'cptr':       cptr,
                'rights':     rights,
                'rights_str': decode_rights(rights),
                'kind':       kind,
                'kind_str':   decode_kind(kind),
                'owner_pd':   owner_pd,
                'granted_to': granted_to,
            })
            pos += CAP_ENTRY_SIZE

        self.net_active,  = struct.unpack_from('<I', d, pos)
        self.net_denials, = struct.unpack_from('<I', d, pos + 4)
        pos += 8

        # Signature: 64 bytes
        sig = d[pos:pos + 64]
        self.sig_key_id = sig[0:8]
        self.sig_hash   = sig[8:40]   # First 32 bytes of SHA-512

        # Verify key_id sentinel
        if self.sig_key_id != ATTEST_KEY_ID:
            self.error = (f'Unknown key_id: {self.sig_key_id.hex()} '
                         f'(expected {ATTEST_KEY_ID.hex()})')
            return

        # Verify SHA-512 of body (everything before signature)
        body = d[:pos]
        full_hash = sha512_of(body)
        self.computed_hash = full_hash[:32]

        if self.computed_hash != self.sig_hash:
            self.error = (f'SHA-512 mismatch!\n'
                         f'  stored:   {self.sig_hash.hex()}\n'
                         f'  computed: {self.computed_hash.hex()}')
            return

        self.valid = True

    def to_dict(self) -> dict:
        return {
            'source':       self.source,
            'valid':        self.valid,
            'error':        self.error or None,
            'magic':        f'0x{self.magic:08X}',
            'version':      self.version,
            'boot_tick':    self.boot_tick,
            'active_caps':  self.active_caps,
            'total_slots':  self.total_slots,
            'audit_seq':    self.audit_seq,
            'net_active':   self.net_active,
            'net_denials':  self.net_denials,
            'sig_key_id':   self.sig_key_id.hex(),
            'sig_hash':     self.sig_hash.hex(),
            'computed_hash': self.computed_hash.hex() if self.computed_hash else None,
            'caps':         self.caps,
        }

    def summary(self) -> str:
        lines = []
        status = '✅ VALID' if self.valid else f'❌ INVALID — {self.error}'
        lines.append(f'Attestation Report: {self.source}')
        lines.append(f'  Status:      {status}')
        if self.magic:
            lines.append(f'  Boot tick:   {self.boot_tick}')
            lines.append(f'  Active caps: {self.active_caps} / {self.total_slots} slots')
            lines.append(f'  Audit seq:   {self.audit_seq}')
            lines.append(f'  Net:         {self.net_active} active slots, {self.net_denials} denials')
            lines.append(f'  SHA-512:     {self.sig_hash.hex()[:32]}…')
        if self.caps:
            lines.append(f'  Capabilities:')
            for c in self.caps:
                granted = f' → pd:{c["granted_to"]}' if c["granted_to"] else ''
                lines.append(f'    [{c["index"]:3d}] cptr=0x{c["cptr"]:08x}  '
                             f'{c["kind_str"]:<16}  {c["rights_str"]:<20}  '
                             f'owner=pd:{c["owner_pd"]}{granted}')
        return '\n'.join(lines)


# ─── AgentFS fetch ────────────────────────────────────────────────────────────

def fetch_from_agentfs(base_url: str, token: str = '') -> list[bytes]:
    """
    Fetch capability attestation objects from AgentFS (HTTP API on port 8791).

    AgentFS serves objects by kind via:
      GET /objects?kind=0xCA   → JSON list of object IDs
      GET /objects/<id>        → raw object bytes
    """
    import urllib.request
    import urllib.error

    headers = {}
    if token:
        headers['Authorization'] = f'Bearer {token}'

    def do_get(url):
        req = urllib.request.Request(url, headers=headers)
        return urllib.request.urlopen(req, timeout=10).read()

    # List attestation objects (kind=0xCA)
    list_url = f'{base_url.rstrip("/")}/objects?kind=0xCA'
    try:
        raw = do_get(list_url)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            # Try legacy path
            list_url = f'{base_url.rstrip("/")}/api/objects?kind=202'
            raw = do_get(list_url)
        else:
            raise

    obj_list = json.loads(raw.decode())
    if isinstance(obj_list, dict):
        obj_ids = obj_list.get('objects', obj_list.get('ids', []))
    else:
        obj_ids = obj_list

    reports = []
    for obj_id in obj_ids:
        obj_url = f'{base_url.rstrip("/")}/objects/{obj_id}'
        try:
            data = do_get(obj_url)
            reports.append((str(obj_id), data))
        except Exception as e:
            print(f'  warn: could not fetch object {obj_id}: {e}', file=sys.stderr)

    return reports


# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description='Verify agentOS capability attestation reports',
        epilog='Returns exit code 0 if all reports are valid, 1 if any fail.'
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--file',     help='Verify a local .att binary file')
    group.add_argument('--dir',      help='Verify all .att files in a directory')
    group.add_argument('--agentfs',  help='Fetch from AgentFS (e.g. http://sparky:8791)',
                       metavar='URL')
    group.add_argument('--stdin',    action='store_true', help='Read binary report from stdin')

    parser.add_argument('--json',   action='store_true', help='Output JSON instead of text')
    parser.add_argument('--quiet',  action='store_true', help='Only show failures')
    parser.add_argument('--token',  help='AgentFS auth token (or AGENTFS_TOKEN env var)',
                        default=os.environ.get('AGENTFS_TOKEN', ''))
    args = parser.parse_args()

    agentfs_url = args.agentfs or os.environ.get('AGENTFS_URL', 'http://sparky:8791')

    reports_raw: list[tuple[str, bytes]] = []

    if args.stdin:
        data = sys.stdin.buffer.read()
        reports_raw = [('<stdin>', data)]

    elif args.file:
        with open(args.file, 'rb') as f:
            data = f.read()
        reports_raw = [(args.file, data)]

    elif args.dir:
        import glob
        paths = sorted(glob.glob(os.path.join(args.dir, '*.att')))
        if not paths:
            print(f'No .att files found in {args.dir}', file=sys.stderr)
            sys.exit(1)
        for p in paths:
            with open(p, 'rb') as f:
                reports_raw.append((p, f.read()))

    elif args.agentfs:
        print(f'Fetching from AgentFS: {agentfs_url}')
        try:
            reports_raw = fetch_from_agentfs(agentfs_url, args.token)
        except Exception as e:
            print(f'Error fetching from AgentFS: {e}', file=sys.stderr)
            sys.exit(1)
        if not reports_raw:
            print('No attestation reports found in AgentFS.')
            sys.exit(0)

    # Decode + verify
    reports = [AttestationReport(data, source) for source, data in reports_raw]

    all_valid = all(r.valid for r in reports)
    failed    = [r for r in reports if not r.valid]

    if args.json:
        output = {'all_valid': all_valid, 'reports': [r.to_dict() for r in reports]}
        print(json.dumps(output, indent=2))
    else:
        for r in reports:
            if args.quiet and r.valid:
                continue
            print(r.summary())
            print()

        total = len(reports)
        n_ok  = sum(1 for r in reports if r.valid)
        print(f'Summary: {n_ok}/{total} reports valid')
        if failed:
            print(f'Failed:')
            for r in failed:
                print(f'  {r.source}: {r.error}')

    sys.exit(0 if all_valid else 1)


if __name__ == '__main__':
    main()
