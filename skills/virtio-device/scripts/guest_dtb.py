#!/usr/bin/env python3
"""HTML report of guest DTS virtio-mmio nodes vs agentOS emulated net IPA."""
from __future__ import annotations

import html
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
OVERLAY = ROOT / "kernel" / "agentos-root-task" / "ubuntu-iso-overlay.dts.in"
EMU_IPA = "0xa010000"

NODE_RE = re.compile(
    r"virtio_mmio@([0-9a-fA-F]+)\s*\{[^}]*reg\s*=\s*<([^>]+)>",
    re.S,
)


def main() -> int:
    text = OVERLAY.read_text(encoding="utf-8")
    rows = []
    found_emu = False
    for m in NODE_RE.finditer(text):
        node = m.group(1).lower()
        reg = " ".join(m.group(2).split())
        kind = "emulated-agentos" if node.replace("0x", "") in (
            "a010000",
            "0a010000",
        ) or "a010000" in node else "qemu-passthrough-crutch"
        if kind == "emulated-agentos":
            found_emu = True
        rows.append(
            f"<tr class='{kind}'><td>@{html.escape(node)}</td>"
            f"<td><code>{html.escape(reg)}</code></td>"
            f"<td>{kind}</td></tr>"
        )
    status = (
        "<p><b>OK:</b> guest DTB includes agentOS emulated virtio-net.</p>"
        if found_emu
        else "<p><b>GAP:</b> no emulated virtio-net node yet "
        f"(want IPA {EMU_IPA}).</p>"
    )
    sys.stdout.write(
        f"""<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>guest virtio-mmio</title>
<style>
body {{ font-family: sans-serif; max-width: 52rem; margin: 1.5rem auto; }}
tr.emulated-agentos {{ background: #e8f5e9; }}
tr.qemu-passthrough-crutch {{ background: #fff8e1; }}
</style></head><body>
<h1>Guest virtio-mmio nodes</h1>
<p>Source: <code>kernel/agentos-root-task/ubuntu-iso-overlay.dts.in</code></p>
{status}
<table border="1" cellpadding="6">
<thead><tr><th>node</th><th>reg</th><th>role</th></tr></thead>
<tbody>{''.join(rows)}</tbody></table>
</body></html>
"""
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
