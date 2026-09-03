#!/usr/bin/env python3
"""HTML dump of the agentOS net_virt shared-memory layout."""
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
HDR = ROOT / "platform" / "include" / "platform" / "net_layout.h"


def macros(text: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for m in re.finditer(
        r"#define\s+(AOS_NET_[A-Z0-9_]+)\s+(.+)", text
    ):
        out[m.group(1)] = m.group(2).strip()
    return out


def main() -> int:
    text = HDR.read_text(encoding="utf-8")
    mac = macros(text)
    rows = "".join(
        f"<tr><td><code>{k}</code></td><td><code>{v}</code></td></tr>"
        for k, v in mac.items()
    )
    sys.stdout.write(
        f"""<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>net_virt layout</title>
<style>
body {{ font-family: sans-serif; max-width: 48rem; margin: 1.5rem auto; }}
code {{ font-size: 0.95rem; }}
</style></head><body>
<h1>agentOS net_virt queue layout</h1>
<p>Source: <code>platform/include/platform/net_layout.h</code></p>
<p>This region is the virtualizer/client ABI. Guest virtio DMA stays in
guest RAM and is copied by the VMM into these buffers.</p>
<table border="1" cellpadding="6"><thead><tr><th>macro</th><th>value</th></tr>
</thead><tbody>{rows}</tbody></table>
</body></html>
"""
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
