#!/usr/bin/env python3
"""HTML report of guest DTS virtio-mmio nodes vs agentOS emulated net IPA."""
from __future__ import annotations

import html
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
EMU_IPA = "0xa010000"
OVERLAYS = (
    ROOT / "kernel" / "agentos-root-task" / "ubuntu-iso-overlay.dts.in",
    ROOT / "libvmm" / "examples" / "simple" / "board" / "qemu_virt_aarch64" / "overlay.dts",
    ROOT / "libvmm" / "examples" / "simple" / "board" / "qemu_virt_aarch64" / "ubuntu-overlay.dts",
)

NODE_RE = re.compile(
    r"virtio_mmio@([0-9a-fA-F]+)\s*\{[^}]*reg\s*=\s*<([^>]+)>",
    re.S,
)


def classify(node: str) -> str:
    n = node.replace("0x", "").lower()
    if n in ("a010000", "0a010000") or "a010000" in n:
        return "emulated-agentos"
    return "qemu-passthrough-crutch"


def scan(path: pathlib.Path) -> tuple[list[str], bool]:
    text = path.read_text(encoding="utf-8")
    rows = []
    found_emu = False
    for m in NODE_RE.finditer(text):
        node = m.group(1).lower()
        reg = " ".join(m.group(2).split())
        kind = classify(node)
        if kind == "emulated-agentos":
            found_emu = True
        rel = path.relative_to(ROOT)
        rows.append(
            f"<tr class='{kind}'><td><code>{html.escape(str(rel))}</code></td>"
            f"<td>@{html.escape(node)}</td>"
            f"<td><code>{html.escape(reg)}</code></td>"
            f"<td>{kind}</td></tr>"
        )
    return rows, found_emu


def main() -> int:
    rows: list[str] = []
    missing: list[str] = []
    for overlay in OVERLAYS:
        file_rows, found = scan(overlay)
        rows.extend(file_rows)
        if not found:
            missing.append(str(overlay.relative_to(ROOT)))
    if missing:
        status = (
            "<p><b>GAP:</b> missing emulated virtio-net node in "
            + ", ".join(f"<code>{html.escape(m)}</code>" for m in missing)
            + f" (want IPA {EMU_IPA}).</p>"
        )
    else:
        status = (
            "<p><b>OK:</b> every Linux overlay DTB includes agentOS "
            "emulated virtio-net at IPA 0xa010000.</p>"
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
<p>Sources: ubuntu-iso-overlay.dts.in, overlay.dts, ubuntu-overlay.dts</p>
{status}
<table border="1" cellpadding="6">
<thead><tr><th>file</th><th>node</th><th>reg</th><th>role</th></tr></thead>
<tbody>{''.join(rows)}</tbody></table>
</body></html>
"""
    )
    return 1 if missing else 0


if __name__ == "__main__":
    raise SystemExit(main())
