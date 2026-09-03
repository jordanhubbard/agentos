#!/usr/bin/env python3
"""HTML: intended platform vs current live AArch64 PD set."""
from __future__ import annotations

import html
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
DESC = ROOT / "kernel" / "agentos-root-task" / "src" / "system_desc_aarch64.c"

INTENDED = [
    "root task",
    "serial_pd (driver)",
    "net_virt (mux)",
    "linux_vmm (emulated virtio)",
    "freebsd_vmm (same VMM, other payload)",
    "nic_drv (owns host virtio-net / real NIC)",
    "blk_drv + blk_virt",
]


def live() -> list[str]:
    text = DESC.read_text(encoding="utf-8", errors="replace")
    return re.findall(
        r'\.name\s*=\s*"([^"]+)"\s*,\s*\n\s*\.elf_path',
        text,
    )


def main() -> int:
    names = live()
    intended_rows = "".join(f"<li>{html.escape(x)}</li>" for x in INTENDED)
    live_rows = "".join(f"<li><code>{html.escape(n)}</code></li>" for n in names)
    sys.stdout.write(
        f"""<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>compose agentOS</title>
<style>
body {{ font-family: sans-serif; max-width: 52rem; margin: 1.5rem auto; }}
.grid {{ display: grid; grid-template-columns: 1fr 1fr; gap: 1.5rem; }}
</style></head><body>
<h1>Compose agentOS</h1>
<p>Intended TCB vs what <code>system_desc_aarch64.c</code> still starts.
Museum PDs should shrink, not grow.</p>
<div class="grid">
<section><h2>Intended blocks</h2><ul>{intended_rows}</ul></section>
<section><h2>Live PDs ({len(names)})</h2><ul>{live_rows}</ul></section>
</div>
</body></html>
"""
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
