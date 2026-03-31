#!/usr/bin/env python3
"""
test_attest_verify.py — Unit tests for attest_verify.py

Generates synthetic attestation reports in NATT v1 format,
passes them through AttestationReport decoder, verifies correct
validation of good/bad/truncated reports.

Run:
  python3 tools/test_attest_verify.py
"""

import struct
import hashlib
import sys
import os

# Add parent dir so we can import attest_verify
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from attest_verify import AttestationReport, ATTEST_MAGIC, ATTEST_VERSION, ATTEST_KEY_ID

PASS = 0
FAIL = 0

def ok(desc):
    global PASS
    PASS += 1
    print(f'  PASS: {desc}')

def fail(desc, detail=''):
    global FAIL
    FAIL += 1
    print(f'  FAIL: {desc}' + (f' — {detail}' if detail else ''))


def sha512(data):
    return hashlib.sha512(data).digest()


def make_report(boot_tick=12345678, caps=None, net_active=3, net_denials=7, corrupt_sig=False):
    """Build a valid NATT v1 attestation report."""
    if caps is None:
        caps = [
            (0x00000100, 0x01, 0x01, 1, 2),   # (cptr, rights, kind, owner_pd, granted_to)
            (0x00000200, 0x03, 0x02, 1, 0),
        ]

    body = b''
    # Header (32 bytes)
    body += struct.pack('<II', ATTEST_MAGIC, ATTEST_VERSION)   # 0: magic, version
    body += struct.pack('<Q', boot_tick)                        # 8: boot_tick
    body += struct.pack('<I', len(caps))                        # 16: active_cap_count
    body += struct.pack('<I', 256)                              # 20: total_cap_slots
    body += struct.pack('<Q', 99)                               # 24: audit_seq

    # Cap entries
    for cptr, rights, kind, owner, granted in caps:
        body += struct.pack('<IIIII', cptr, rights, kind, owner, granted)

    # Net summary
    body += struct.pack('<II', net_active, net_denials)

    # Signature
    full_hash = sha512(body)
    sig = ATTEST_KEY_ID + full_hash[:32] + b'\x00' * 24
    assert len(sig) == 64

    if corrupt_sig:
        sig = ATTEST_KEY_ID + b'\xFF' * 32 + b'\x00' * 24

    return body + sig


# ─── Tests ─────────────────────────────────────────────────────────────────────

print('=== attest_verify.py unit tests ===')
print()

# 1. Valid report with 2 caps
r = AttestationReport(make_report(), 'test:valid_2caps')
if r.valid:
    ok('valid report with 2 caps')
else:
    fail('valid report with 2 caps', r.error)

if r.active_caps == 2:
    ok('active_caps == 2')
else:
    fail('active_caps', f'got {r.active_caps}')

if r.boot_tick == 12345678:
    ok('boot_tick == 12345678')
else:
    fail('boot_tick', f'got {r.boot_tick}')

if r.net_active == 3 and r.net_denials == 7:
    ok('net stats (3 active, 7 denials)')
else:
    fail('net stats', f'active={r.net_active}, denials={r.net_denials}')

if r.audit_seq == 99:
    ok('audit_seq == 99')
else:
    fail('audit_seq', f'got {r.audit_seq}')

# 2. Valid report with 0 caps
r0 = AttestationReport(make_report(caps=[]), 'test:zero_caps')
if r0.valid:
    ok('valid report with 0 caps')
else:
    fail('valid report with 0 caps', r0.error)

if r0.active_caps == 0:
    ok('active_caps == 0')
else:
    fail('active_caps == 0', f'got {r0.active_caps}')

# 3. Corrupted signature
rc = AttestationReport(make_report(corrupt_sig=True), 'test:corrupt_sig')
if not rc.valid:
    ok('corrupt signature correctly rejected')
else:
    fail('corrupt signature should be rejected')
if 'mismatch' in rc.error.lower():
    ok('error mentions hash mismatch')
else:
    fail('error should mention mismatch', rc.error)

# 4. Wrong magic
bad_magic = make_report()
bad_magic = b'\xDE\xAD\xBE\xEF' + bad_magic[4:]
rm = AttestationReport(bad_magic, 'test:bad_magic')
if not rm.valid:
    ok('bad magic correctly rejected')
else:
    fail('bad magic should be rejected')

# 5. Truncated report
rt = AttestationReport(make_report()[:40], 'test:truncated')
if not rt.valid:
    ok('truncated report rejected')
else:
    fail('truncated report should be rejected')

# 6. Wrong key_id
rk = AttestationReport(
    make_report()[:len(make_report())-64] +
    b'\xFF' * 8 + make_report()[-56:],
    'test:bad_key_id'
)
if not rk.valid:
    ok('wrong key_id rejected')
else:
    fail('wrong key_id should be rejected')

# 7. Cap fields decoded correctly
r2 = AttestationReport(make_report(), 'test:cap_fields')
cap0 = r2.caps[0]
if cap0['cptr'] == 0x100:
    ok('cap[0].cptr == 0x100')
else:
    fail('cap[0].cptr', f'got {cap0["cptr"]!r}')
if cap0['rights_str'] == 'read':
    ok('cap[0].rights_str == "read"')
else:
    fail('cap[0].rights_str', f'got {cap0["rights_str"]!r}')
if cap0['kind_str'] == 'endpoint':
    ok('cap[0].kind_str == "endpoint"')
else:
    fail('cap[0].kind_str', f'got {cap0["kind_str"]!r}')
if cap0['owner_pd'] == 1 and cap0['granted_to'] == 2:
    ok('cap[0].owner/granted')
else:
    fail('cap[0].owner/granted', f'owner={cap0["owner_pd"]}, granted={cap0["granted_to"]}')

# 8. to_dict round-trip
d = r.to_dict()
if d['valid'] and d['magic'] == '0x4E415454' and len(d['caps']) == 2:
    ok('to_dict() round-trip')
else:
    fail('to_dict() round-trip', str(d.get('valid')))

# 9. Large report (MAX_CAPS = 256 entries)
big_caps = [(i * 0x100, 0x01 | (i % 4), i % 10 + 1, 1, 0) for i in range(256)]
rbig = AttestationReport(make_report(caps=big_caps, boot_tick=9999999), 'test:256_caps')
if rbig.valid:
    ok('256-cap report valid')
else:
    fail('256-cap report', rbig.error)
if rbig.active_caps == 256:
    ok('active_caps == 256')
else:
    fail('active_caps 256', f'got {rbig.active_caps}')

# ─── Summary ─────────────────────────────────────────────────────────────────

print()
print(f'Results: {PASS} passed, {FAIL} failed')
sys.exit(0 if FAIL == 0 else 1)
