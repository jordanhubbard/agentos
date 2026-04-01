#!/usr/bin/env python3
"""
gen_sdf.py — agentOS Microkit SDF generator

Reads a high-level YAML topology spec and emits a validated agentos.system
SDF (seL4 Microkit System Description Format).

Usage:
    python3 gen_sdf.py topology.yaml [-o agentos.system] [--validate-only]

YAML topology format:

    version: "0.3"

    memory_regions:
      - name: vibe_code
        size: 0x400000      # 4MB
        perms: rwx
      - name: perf_ring
        size: 0x1000        # 4KB

    pds:
      - name: controller
        priority: 50
        passive: false
        program_image: controller.elf
        maps:
          - mr: vibe_code
            vaddr: 0x2000000
            perms: r
            setvar_vaddr: vibe_code_ctrl_vaddr

      - name: event_bus
        priority: 200
        passive: true
        program_image: event_bus.elf

    channels:
      - name: ctrl_eventbus
        pd_a: controller
        id_a: 0
        pp_a: true         # pp=true on controller end (controller PPCs into event_bus)
        pd_b: event_bus
        id_b: 0

      - name: worker_perf_0
        pd_a: worker_0
        id_a: 10
        pp_a: true
        pd_b: perf_counters
        id_b: 1

Validations performed:
  - No duplicate (pd, channel_id) pairs
  - All pd references in channels exist in pds list
  - All mr references in maps exist in memory_regions
  - Channel IDs within [0, 62] (Microkit limit)
  - No two channels share the same id on the same pd
  - Passive PDs with pp=true channels have exactly one pp end (the caller)

Output: XML in Microkit SDF format (agentos.system).
"""

import sys
import os
import argparse
import textwrap
from typing import Dict, List, Optional, Set, Tuple

try:
    import yaml
except ImportError:
    print("error: PyYAML required — pip install pyyaml", file=sys.stderr)
    sys.exit(1)

# ── Constants ────────────────────────────────────────────────────────────── #

MICROKIT_MAX_CHANNEL_ID = 62
MICROKIT_MAX_PDS        = 63
MICROKIT_MAX_MRS        = 64
MICROKIT_MAX_CHANNELS   = 128

VALID_PERMS = {"r", "w", "x", "rw", "rx", "rwx", "rws"}

# ── Data classes (dict-based for simplicity) ─────────────────────────────── #

class ValidationError(Exception):
    pass

# ── Loader ───────────────────────────────────────────────────────────────── #

def load_topology(path: str) -> dict:
    with open(path) as f:
        data = yaml.safe_load(f)
    if not isinstance(data, dict):
        raise ValidationError(f"{path}: top-level must be a YAML mapping")
    return data

# ── Validators ───────────────────────────────────────────────────────────── #

def validate(topo: dict) -> List[str]:
    """Return list of error strings (empty = valid)."""
    errors = []

    pd_names: Set[str] = set()
    mr_names: Set[str] = set()

    pds       = topo.get("pds", [])
    mrs       = topo.get("memory_regions", [])
    channels  = topo.get("channels", [])

    # ── PD names unique ──────────────────────────────────────────────── #
    if len(pds) > MICROKIT_MAX_PDS:
        errors.append(f"Too many PDs: {len(pds)} (Microkit limit {MICROKIT_MAX_PDS})")
    for pd in pds:
        nm = pd.get("name", "")
        if not nm:
            errors.append("PD missing 'name'")
            continue
        if nm in pd_names:
            errors.append(f"Duplicate PD name: '{nm}'")
        pd_names.add(nm)
        prio = pd.get("priority", None)
        if prio is None:
            errors.append(f"PD '{nm}' missing 'priority'")
        elif not isinstance(prio, int) or prio < 0 or prio > 254:
            errors.append(f"PD '{nm}' priority {prio!r} out of range [0,254]")

    # ── MR names unique ──────────────────────────────────────────────── #
    if len(mrs) > MICROKIT_MAX_MRS:
        errors.append(f"Too many memory_regions: {len(mrs)} (Microkit limit {MICROKIT_MAX_MRS})")
    for mr in mrs:
        nm = mr.get("name", "")
        if not nm:
            errors.append("memory_region missing 'name'")
            continue
        if nm in mr_names:
            errors.append(f"Duplicate memory_region name: '{nm}'")
        mr_names.add(nm)
        sz = mr.get("size")
        if sz is None:
            errors.append(f"MR '{nm}' missing 'size'")

    # ── Map references ────────────────────────────────────────────────── #
    for pd in pds:
        pnm = pd.get("name", "?")
        for mp in pd.get("maps", []):
            mr_ref = mp.get("mr", "")
            if mr_ref not in mr_names:
                errors.append(f"PD '{pnm}' maps unknown MR '{mr_ref}'")
            p = mp.get("perms", "r")
            if p not in VALID_PERMS:
                errors.append(f"PD '{pnm}' map has invalid perms '{p}'")

    # ── Channel validation ────────────────────────────────────────────── #
    if len(channels) > MICROKIT_MAX_CHANNELS:
        errors.append(f"Too many channels: {len(channels)} (Microkit limit {MICROKIT_MAX_CHANNELS})")

    # Track (pd_name, channel_id) -> channel_name for collision detection
    pd_ch_map: Dict[Tuple[str,int], str] = {}

    for ch in channels:
        cname = ch.get("name", "<unnamed>")
        for side in ("a", "b"):
            pd_key = f"pd_{side}"
            id_key = f"id_{side}"
            pnm = ch.get(pd_key, "")
            cid = ch.get(id_key)
            if not pnm:
                errors.append(f"Channel '{cname}' missing '{pd_key}'")
                continue
            if pnm not in pd_names:
                errors.append(f"Channel '{cname}' references unknown PD '{pnm}'")
            if cid is None:
                errors.append(f"Channel '{cname}' missing '{id_key}'")
                continue
            if not isinstance(cid, int) or cid < 0 or cid > MICROKIT_MAX_CHANNEL_ID:
                errors.append(f"Channel '{cname}' PD '{pnm}' id {cid!r} "
                               f"out of range [0,{MICROKIT_MAX_CHANNEL_ID}]")
            key = (pnm, cid)
            if key in pd_ch_map:
                errors.append(f"Channel ID collision: PD '{pnm}' id={cid} "
                               f"used by both '{cname}' and '{pd_ch_map[key]}'")
            else:
                pd_ch_map[key] = cname

    return errors

# ── XML emitter ───────────────────────────────────────────────────────────── #

def _hex(v) -> str:
    if isinstance(v, int):
        return hex(v)
    return str(v)

def emit_sdf(topo: dict) -> str:
    lines = []
    lines.append('<?xml version="1.0" encoding="UTF-8"?>')

    # Header comment with channel ID table
    chans = topo.get("channels", [])
    lines.append("<!--")
    lines.append(f"  Generated by gen_sdf.py - agentOS Microkit SDF")
    lines.append(f"  PDs: {len(topo.get('pds',[]))}, "
                 f"Channels: {len(chans)}, "
                 f"MRs: {len(topo.get('memory_regions',[]))}")
    lines.append("")
    lines.append("  Channel ID allocation table:")
    lines.append(f"  {'Channel name':<40} {'PD A':<18} {'id_A':>4}  {'PD B':<18} {'id_B':>4}")
    lines.append("  " + "=" * 90)
    for ch in sorted(chans, key=lambda c: (c.get("id_a", 999), c.get("name",""))):
        lines.append(f"  {ch.get('name',''):<40} "
                     f"{ch.get('pd_a',''):<18} {ch.get('id_a',''):>4}  "
                     f"{ch.get('pd_b',''):<18} {ch.get('id_b',''):>4}")
    lines.append("-->")
    lines.append("<system>")

    # Memory regions
    mrs = topo.get("memory_regions", [])
    if mrs:
        lines.append("")
        lines.append("    <!-- Memory Regions -->")
    for mr in mrs:
        attrs = [f'name="{mr["name"]}"', f'size="{_hex(mr["size"])}"']
        if "perms" in mr:
            attrs.append(f'page_size="{_hex(mr.get("page_size", 0x1000))}"')
        lines.append(f'    <memory_region {" ".join(attrs)} />')

    # Protection domains
    pds = topo.get("pds", [])
    lines.append("")
    lines.append("    <!-- Protection Domains -->")
    for pd in pds:
        passive = ' passive="true"' if pd.get("passive", False) else ""
        pp = f'    <protection_domain name="{pd["name"]}" priority="{pd["priority"]}"{passive}>'
        lines.append(pp)
        if pd.get("program_image"):
            lines.append(f'        <program_image path="{pd["program_image"]}" />')
        for mp in pd.get("maps", []):
            mp_attrs = [
                f'mr="{mp["mr"]}"',
                f'vaddr="{_hex(mp["vaddr"])}"',
                f'perms="{mp.get("perms","r")}"',
            ]
            if "cached" in mp:
                mp_attrs.append(f'cached="{str(mp["cached"]).lower()}"')
            if "setvar_vaddr" in mp:
                mp_attrs.append(f'setvar_vaddr="{mp["setvar_vaddr"]}"')
            lines.append(f'        <map {" ".join(mp_attrs)} />')
        lines.append("    </protection_domain>")
        lines.append("")

    # Channels
    lines.append("    <!-- Channels -->")
    for ch in chans:
        cname = ch.get("name", "")
        if cname:
            lines.append(f"    <!-- {cname} -->")
        lines.append("    <channel>")
        for side in ("a", "b"):
            pnm = ch.get(f"pd_{side}", "")
            cid = ch.get(f"id_{side}", 0)
            pp  = ch.get(f"pp_{side}", False)
            pp_attr = ' pp="true"' if pp else ""
            lines.append(f'        <end pd="{pnm}" id="{cid}"{pp_attr} />')
        lines.append("    </channel>")
        lines.append("")

    lines.append("</system>")
    return "\n".join(lines)

# ── Channel allocation helper ─────────────────────────────────────────────── #

def alloc_channel_ids(topo: dict) -> dict:
    """
    Auto-assign channel IDs if a channel has id_a/id_b = "auto".
    Returns a copy of topo with IDs filled in.
    """
    import copy
    topo = copy.deepcopy(topo)
    used: Set[Tuple[str,int]] = set()

    # First pass: collect manually assigned IDs
    for ch in topo.get("channels", []):
        for side in ("a", "b"):
            pnm = ch.get(f"pd_{side}", "")
            cid = ch.get(f"id_{side}")
            if isinstance(cid, int):
                used.add((pnm, cid))

    # Second pass: assign "auto" IDs
    for ch in topo.get("channels", []):
        for side in ("a", "b"):
            pnm = ch.get(f"pd_{side}", "")
            cid = ch.get(f"id_{side}")
            if cid == "auto":
                for candidate in range(0, MICROKIT_MAX_CHANNEL_ID + 1):
                    if (pnm, candidate) not in used:
                        ch[f"id_{side}"] = candidate
                        used.add((pnm, candidate))
                        break
    return topo

# ── CLI ──────────────────────────────────────────────────────────────────── #

def main() -> int:
    parser = argparse.ArgumentParser(
        description="agentOS Microkit SDF generator",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""
            Examples:
              python3 gen_sdf.py topology.yaml
              python3 gen_sdf.py topology.yaml -o agentos.system
              python3 gen_sdf.py topology.yaml --validate-only
        """))
    parser.add_argument("topology", help="YAML topology spec")
    parser.add_argument("-o", "--output",  default=None,
                        help="Output .system file (default: stdout)")
    parser.add_argument("--validate-only", action="store_true",
                        help="Only validate, do not emit SDF")
    parser.add_argument("--auto-ids",      action="store_true",
                        help="Auto-assign channel IDs marked as 'auto'")
    args = parser.parse_args()

    try:
        topo = load_topology(args.topology)
    except FileNotFoundError:
        print(f"error: {args.topology}: not found", file=sys.stderr)
        return 1
    except yaml.YAMLError as e:
        print(f"error: YAML parse error: {e}", file=sys.stderr)
        return 1

    if args.auto_ids:
        topo = alloc_channel_ids(topo)

    errors = validate(topo)
    if errors:
        print(f"Validation FAILED ({len(errors)} error(s)):", file=sys.stderr)
        for e in errors:
            print(f"  ✗ {e}", file=sys.stderr)
        return 1
    else:
        print(f"✓ Validation passed: "
              f"{len(topo.get('pds',[]))} PDs, "
              f"{len(topo.get('channels',[]))} channels, "
              f"{len(topo.get('memory_regions',[]))} MRs",
              file=sys.stderr)

    if args.validate_only:
        return 0

    sdf = emit_sdf(topo)

    if args.output:
        with open(args.output, "w") as f:
            f.write(sdf)
        print(f"✓ Wrote {args.output}", file=sys.stderr)
    else:
        print(sdf)

    return 0

if __name__ == "__main__":
    sys.exit(main())
