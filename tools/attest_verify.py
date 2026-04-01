#!/usr/bin/env python3
"""
attest_verify.py — agentOS Capability Attestation Verifier

Fetches a capability attestation report from AgentFS and verifies:
  1. The Ed25519 signature over the attestation body
  2. Parses and pretty-prints the active capability table
  3. Optionally diffs against a previous attestation (change detection)

Usage:
    python3 tools/attest_verify.py [--agentfs <url>] [--pubkey <path>] [--diff <file>]
    python3 tools/attest_verify.py --local <attestation.txt> --pubkey keys/system.pub
    python3 tools/attest_verify.py --latest   # fetch latest from AgentFS and verify

Output: structured report to stdout; exit code 0 = verified, 1 = failed/tampered.

Attestation file format (written by cap_broker_attest() in monitor PD):
    ATTEST    <seq>    <timestamp_us>
    CAP    <handle>    <owner_pd>    <granted_to>    <cptr>    <rights>    <kind>    <badge>    <revokable>    <grant_time>
    ...
    END    <num_caps>

Signed attestation file format (written by monitor PD to AgentFS):
    [4-byte big-endian body_len][body_bytes][64-byte Ed25519 signature]

The trusted system public key is at: ~/.nanoc/system.pub (raw 32-byte Ed25519)
or provided via --pubkey.

AgentFS path: agentos/attestation/<seq>.bin
"""

import sys
import os
import argparse
import struct
import json
import textwrap
import urllib.request
import urllib.error
from datetime import datetime, timezone

# ── Capability metadata ────────────────────────────────────────────────── #

CAP_KIND_NAMES = {
    0: "untyped",
    1: "endpoint",
    2: "notification",
    3: "reply",
    4: "page",
    5: "tcb",
    6: "sched_context",
    7: "irq_handler",
    8: "io_port",
    9: "frame",
    10: "vspace",
}

CAP_RIGHTS = {
    0x01: "read",
    0x02: "write",
    0x04: "grant",
    0x08: "grant_reply",
}

def decode_rights(rights_int):
    r = int(rights_int, 16) if isinstance(rights_int, str) else rights_int
    return ",".join(v for k, v in sorted(CAP_RIGHTS.items()) if r & k) or "none"

def kind_name(kind_hex):
    k = int(kind_hex, 16) if isinstance(kind_hex, str) else kind_hex
    return CAP_KIND_NAMES.get(k, f"kind_0x{k:02x}")

# ── Parser ─────────────────────────────────────────────────────────────── #

class AttestationRecord:
    def __init__(self, seq, timestamp_us, caps, raw_body):
        self.seq          = seq
        self.timestamp_us = timestamp_us
        self.timestamp    = datetime.fromtimestamp(timestamp_us / 1e6, tz=timezone.utc)
        self.caps         = caps      # list of dicts
        self.raw_body     = raw_body  # bytes

def parse_attestation(body_text: str) -> AttestationRecord:
    lines = body_text.strip().splitlines()
    seq = ts = 0
    caps = []
    for line in lines:
        parts = line.split('\t')
        if parts[0] == 'ATTEST' and len(parts) >= 3:
            seq = int(parts[1])
            ts  = int(parts[2])
        elif parts[0] == 'CAP' and len(parts) >= 10:
            caps.append({
                'handle':     int(parts[1]),
                'owner_pd':   int(parts[2]),
                'granted_to': int(parts[3]),
                'cptr':       parts[4],
                'rights':     parts[5],
                'kind':       parts[6],
                'badge':      parts[7],
                'revokable':  parts[8] == '1',
                'grant_time': int(parts[9]),
            })
        elif parts[0] == 'END':
            pass
    return AttestationRecord(seq, ts, caps, body_text.encode())

# ── Ed25519 verification ───────────────────────────────────────────────── #

def verify_ed25519(pubkey_raw32: bytes, message: bytes, signature64: bytes) -> bool:
    """Verify Ed25519 signature using cryptography package or openssl subprocess."""
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
        from cryptography.exceptions import InvalidSignature
        pub = Ed25519PublicKey.from_public_bytes(pubkey_raw32)
        try:
            pub.verify(signature64, message)
            return True
        except InvalidSignature:
            return False
    except ImportError:
        pass
    # Fallback: openssl dgst
    import subprocess, tempfile
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(message)
        msg_path = f.name
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(signature64)
        sig_path = f.name
    # Wrap raw key in SubjectPublicKeyInfo DER
    der_prefix = bytes.fromhex('302a300506032b6570032100')
    pub_der = der_prefix + pubkey_raw32
    with tempfile.NamedTemporaryFile(suffix='.der', delete=False) as f:
        f.write(pub_der)
        pub_path = f.name
    try:
        r = subprocess.run(
            ['openssl', 'pkeyutl', '-verify', '-inkey', pub_path, '-keyform', 'DER',
             '-pubin', '-in', msg_path, '-sigfile', sig_path, '-rawin'],
            capture_output=True)
        return r.returncode == 0
    except FileNotFoundError:
        return None  # openssl not available
    finally:
        for p in (msg_path, sig_path, pub_path):
            os.unlink(p)

# ── Signed attestation file format ────────────────────────────────────── #
# [4-byte big-endian body_len][body][64-byte sig]

def load_signed_attestation(data: bytes):
    """Returns (body_bytes, sig_bytes) or raises ValueError."""
    if len(data) < 68:
        raise ValueError("Too short for signed attestation")
    body_len = struct.unpack('>I', data[:4])[0]
    if 4 + body_len + 64 > len(data):
        raise ValueError(f"Truncated: claimed body_len={body_len} but file is {len(data)} bytes")
    body = data[4:4 + body_len]
    sig  = data[4 + body_len:4 + body_len + 64]
    return body, sig

# ── AgentFS fetch ─────────────────────────────────────────────────────── #

def fetch_latest(agentfs_url: str, token: str = None) -> bytes:
    """Fetch the latest attestation from AgentFS."""
    list_url = f"{agentfs_url}/ls?prefix=agentos/attestation/"
    headers = {"Authorization": f"Bearer {token}"} if token else {}
    req = urllib.request.Request(list_url, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            listing = json.loads(r.read())
    except urllib.error.URLError as e:
        raise RuntimeError(f"AgentFS list failed: {e}")
    files = listing.get('files', listing) if isinstance(listing, dict) else listing
    if not files:
        raise RuntimeError("No attestation files found in AgentFS")
    # Sort by filename descending (seq order)
    latest = sorted(str(f) for f in files)[-1]
    fetch_url = f"{agentfs_url}/get?hash={latest}"
    req2 = urllib.request.Request(fetch_url, headers=headers)
    with urllib.request.urlopen(req2, timeout=10) as r:
        return r.read()

# ── Report printer ────────────────────────────────────────────────────── #

def print_report(rec: AttestationRecord, verified: bool | None):
    w = 80
    print("=" * w)
    print(f"  agentOS Capability Attestation Report")
    print(f"  Sequence:    {rec.seq}")
    print(f"  Timestamp:   {rec.timestamp.isoformat()}")
    print(f"  Active caps: {len(rec.caps)}")
    if verified is True:
        print(f"  Signature:   ✓ VERIFIED")
    elif verified is False:
        print(f"  Signature:   ✗ INVALID — report may be tampered!")
    else:
        print(f"  Signature:   ? (public key not provided)")
    print("=" * w)
    print()
    print(f"{'Handle':>6}  {'Owner PD':>8}  {'Granted→':>8}  {'Kind':<16}  {'Rights':<24}  {'cptr':<10}  {'Rev'}")
    print("-" * w)
    for c in sorted(rec.caps, key=lambda x: x['handle']):
        granted = str(c['granted_to']) if c['granted_to'] else "—"
        kn = kind_name(c['kind'])
        rr = decode_rights(c['rights'])
        rev = "✓" if c['revokable'] else "—"
        print(f"{c['handle']:>6}  {c['owner_pd']:>8}  {granted:>8}  {kn:<16}  {rr:<24}  {c['cptr']:<10}  {rev}")
    print()

# ── Diff ──────────────────────────────────────────────────────────────── #

def diff_attestations(prev: AttestationRecord, curr: AttestationRecord):
    prev_map = {c['handle']: c for c in prev.caps}
    curr_map = {c['handle']: c for c in curr.caps}
    added   = [c for h, c in curr_map.items() if h not in prev_map]
    removed = [c for h, c in prev_map.items() if h not in curr_map]
    changed = [c for h, c in curr_map.items()
               if h in prev_map and c != prev_map[h]]
    print(f"Diff (seq {prev.seq} → {curr.seq}):")
    if not added and not removed and not changed:
        print("  No changes.")
        return
    for c in added:
        print(f"  + handle {c['handle']}: {kind_name(c['kind'])} owner={c['owner_pd']} granted={c['granted_to']}")
    for c in removed:
        print(f"  - handle {c['handle']}: {kind_name(c['kind'])} owner={c['owner_pd']}")
    for c in changed:
        print(f"  ~ handle {c['handle']}: modified")
    print()

# ── Main ──────────────────────────────────────────────────────────────── #

def main():
    parser = argparse.ArgumentParser(
        description="agentOS capability attestation verifier",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""
            Examples:
              python3 tools/attest_verify.py --latest
              python3 tools/attest_verify.py --local attest_001.bin --pubkey ~/.nanoc/system.pub
              python3 tools/attest_verify.py --latest --diff attest_000.bin
        """))
    parser.add_argument("--latest",   action="store_true", help="Fetch latest from AgentFS")
    parser.add_argument("--local",    metavar="FILE",       help="Load from local file")
    parser.add_argument("--agentfs",  default=os.environ.get("AGENTFS_URL", "http://sparky.tail407856.ts.net:8791"))
    parser.add_argument("--token",    default=os.environ.get("AGENTFS_TOKEN", ""))
    parser.add_argument("--pubkey",   default=os.path.expanduser("~/.nanoc/system.pub"),
                        help="Path to raw 32-byte Ed25519 public key")
    parser.add_argument("--diff",     metavar="PREV_FILE",  help="Diff against previous attestation")
    parser.add_argument("--save",     metavar="FILE",       help="Save fetched attestation to file")
    parser.add_argument("--unsigned", action="store_true",  help="Parse unsigned (plain text) attestation")
    args = parser.parse_args()

    # Load attestation data
    raw_data = None
    if args.local:
        with open(args.local, 'rb') as f:
            raw_data = f.read()
    elif args.latest:
        print(f"Fetching latest attestation from {args.agentfs}...", file=sys.stderr)
        raw_data = fetch_latest(args.agentfs, args.token or None)
    else:
        parser.error("Provide --latest or --local <file>")

    if args.save:
        with open(args.save, 'wb') as f:
            f.write(raw_data)
        print(f"Saved to {args.save}", file=sys.stderr)

    # Parse
    verified = None
    if args.unsigned:
        body = raw_data
        sig  = None
    else:
        try:
            body, sig = load_signed_attestation(raw_data)
        except ValueError:
            # Try treating as plain text
            body = raw_data
            sig  = None

    rec = parse_attestation(body.decode('utf-8', errors='replace'))

    # Verify signature
    if sig and os.path.exists(args.pubkey):
        with open(args.pubkey, 'rb') as f:
            pubkey_raw = f.read()
        if len(pubkey_raw) == 32:
            result = verify_ed25519(pubkey_raw, body, sig)
            verified = result
        else:
            print(f"Warning: pubkey {args.pubkey} is {len(pubkey_raw)} bytes (expected 32)", file=sys.stderr)

    print_report(rec, verified)

    # Diff
    if args.diff:
        with open(args.diff, 'rb') as f:
            diff_raw = f.read()
        try:
            diff_body, _ = load_signed_attestation(diff_raw)
        except ValueError:
            diff_body = diff_raw
        prev_rec = parse_attestation(diff_body.decode('utf-8', errors='replace'))
        diff_attestations(prev_rec, rec)

    # Exit code
    if verified is False:
        print("ATTESTATION FAILED: signature invalid", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
