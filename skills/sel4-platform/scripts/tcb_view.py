#!/usr/bin/env python3
"""Emit an HTML view of docs/TCB.md vs live AArch64 PD names."""
from __future__ import annotations

import html
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
TCB = ROOT / "docs" / "TCB.md"
DESC = ROOT / "kernel" / "agentos-root-task" / "src" / "system_desc_aarch64.c"

TCB_CORE = {
    "root task",
    "nameserver",
    "log_drain",
    "serial_pd",
    "linux_vmm",
    "freebsd_vmm",
    "vm_manager",
    "cc_pd",
    "fault_handler",
    "controller",
    "net_virt",
    "nic_drv",
    "blk_drv",
    "blk_virt",
    "serial_virt",
}

MUSEUM_MARKERS = (
    "oom_killer",
    "mesh_agent",
    "power_mgr",
    "time_partition",
    "wg_net",
    "pflocal",
    "auth_server",
    "mem_profiler",
    "perf_counters",
    "quota_pd",
    "watchdog",
    "http_svc",
    "spawn_server",
    "exec_server",
    "proc_server",
    "app_manager",
    "app_slot",
    "term_server",
    "vibe_engine",
    "swap_slot",
    "gpu_sched",
)


def live_pd_names() -> list[str]:
    text = DESC.read_text(encoding="utf-8", errors="replace")
    return re.findall(
        r'\.name\s*=\s*"([^"]+)"\s*,\s*\n\s*\.elf_path',
        text,
    )


def classify(name: str) -> str:
    if name in TCB_CORE:
        return "tcb"
    if any(m in name for m in MUSEUM_MARKERS):
        return "museum"
    return "unknown"


def main() -> int:
    names = live_pd_names()
    rows = []
    for n in names:
        kind = classify(n)
        rows.append(
            f"<tr class='{kind}'><td>{html.escape(n)}</td>"
            f"<td>{kind}</td></tr>"
        )
    tcb_text = html.escape(TCB.read_text(encoding="utf-8")[:4000])
    sys.stdout.write(
        f"""<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>agentOS TCB</title>
<style>
body {{ font-family: sans-serif; max-width: 52rem; margin: 1.5rem auto; }}
tr.tcb {{ background: #e8f5e9; }}
tr.museum {{ background: #ffebee; }}
tr.unknown {{ background: #fff8e1; }}
pre {{ white-space: pre-wrap; background: #f4f4f4; padding: 1rem; }}
</style></head><body>
<h1>agentOS TCB vs live AArch64 PDs</h1>
<p>Green = TCB-shaped. Red = museum (do not extend). Amber = not classified.</p>
<table border="1" cellpadding="6"><thead><tr><th>PD</th><th>class</th></tr></thead>
<tbody>{''.join(rows)}</tbody></table>
<h2>docs/TCB.md (excerpt)</h2>
<pre>{tcb_text}</pre>
</body></html>
"""
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
