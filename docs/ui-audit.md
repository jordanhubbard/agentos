# agentOS Console Dashboard — UI Discoverability Audit

**Evaluator persona:** Expert UI designer familiar with virt-manager, VMware Fusion, Proxmox VE, Cockpit. Assessing discoverability for a user who understands virtualisation/OS management but has never used agentOS.

---

## Executive Summary

The agentOS console presents a topology-first interface that prioritises Protection Domain (PD) architecture visualisation at the expense of task-oriented workflows. For a user trained on virt-manager or VMware Fusion the interface requires significant cognitive overhead: the fundamental mental model (isolated security domains vs. virtual machines) is presented without bridge labels, terminology is unexplained at point-of-use, and core workflows (spawning agents, understanding node health) lack discoverability cues.

---

## Scorecard

| Criteria | First Launch | Topology | Console Tiles | Sidebar Nav | Images | Agents | Status |
|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **Affordance** | 2 | 1 | 3 | 3 | 3 | 2 | 2 |
| **Discoverability** | 2 | 1 | 2 | 4 | 2 | 2 | 1 |
| **Feedback** | 2 | 4 | 4 | 4 | 3 | 2 | 2 |
| **Error Recovery** | 1 | 2 | 3 | 4 | 3 | 1 | 1 |
| **Conceptual Mapping** | 1 | 1 | 2 | 3 | 3 | 1 | 1 |
| **Average** | **1.6** | **1.8** | **2.8** | **3.6** | **2.8** | **1.6** | **1.4** |

Topology (1.8) and Agents (1.6) are the most severe failures. Status Indicators (1.4) are nearly opaque. Sidebar Navigation (3.6) is the strongest area.

---

## Top 10 Discoverability Gaps

### 1. CRITICAL — PD Terminology Barrier
- **Location:** `topology.rs:19–44` (node definitions), `topology.rs:176–182` (legend)
- **Gap:** Every node is labelled with PD jargon (event_bus, vibe_engine, swap_slot_0) with no plain-English mapping. The legend explains line types but never defines "PD."
- **Fix:** Add a hover tooltip to every node: `title="Protection Domain — an isolated security boundary, similar to a lightweight container"`. Add an info icon next to the Topology tab label pointing to the Architecture doc.

### 2. CRITICAL — Topology-First Default Hides Workflows
- **Location:** `lib.rs:250` (`current_panel` defaults to `"topology"`)
- **Gap:** New users land on a 24-node graph with no "what can I do?" affordance. virt-manager lands on a VM list with obvious Start/Stop/Create buttons.
- **Fix:** Default to Console tab. On first launch, if QEMU is not running, show a Getting Started banner: "1. Start QEMU — 2. Monitor in Topology — 3. Spawn agents."

### 3. CRITICAL — Agents Panel Has No Spawn Button
- **Location:** `lib.rs:824–828` (agents toolbar contains only Refresh)
- **Gap:** Users cannot discover how to create a new agent. No spawn dialog is rendered from this tab at all.
- **Fix:** Add "＋ Spawn Agent" button next to Refresh. Clicking opens a modal with: agent name, WASM binary dropdown, target slot dropdown, and a one-line description "A WASM agent runs in an isolated swap slot and handles tasks."

### 4. CRITICAL — Images Panel Has No Import Affordance
- **Location:** `images.rs:40–85`
- **Gap:** Empty state reads "Place .img files in the guest-images/ directory" — a filesystem instruction, not a UI action. No download or import button exists.
- **Fix:** Add "＋ Import Image" button and a "⬇ Download Buildroot" shortcut. Change empty state to include action buttons, not filesystem instructions.

### 5. CRITICAL — Console Tiles → Topology Connection Is Opaque
- **Location:** `lib.rs:542–560` (slot picker dialog)
- **Gap:** The slot picker lists "0 — controller", "5 — WASM Worker 0" with no connection to the topology node names or status. Users cannot tell which slots correspond to which graph nodes.
- **Fix:** Group slots by category (Core, Workers, Swap) in the picker. Show inline status badges. Add "View in Topology →" link per slot.

### 6. MAJOR — WebSocket Status Dot Meaning Is Ambiguous
- **Location:** `lib.rs:421–428`; `style.css:.ws-dot`
- **Gap:** "WS — connected" means nothing to a user who doesn't know what WebSocket is in this context.
- **Fix:** Replace "WS" with "Serial Console: Live" / "Serial Console: Offline". Add `title="Connection to QEMU serial port for live logs"`.

### 7. MAJOR — Sidebar Nav Icons Are Cryptic
- **Location:** `lib.rs:322–328` (⬡ ⬜ ◎ ◈ ⬇ ⁇)
- **Gap:** These Unicode symbols are not universally understood. ⬡ for topology is arbitrary; ⁇ for docs is non-standard.
- **Fix:** Replace with text labels always visible at collapsed width (or adopt standard icon set). At minimum change ⁇ → "?" and ⬡ → a graph icon SVG.

### 8. MAJOR — Topology Status Dots Have No Legend
- **Location:** `topology.rs:144–162`; `style.css:~1010–1013`
- **Gap:** Four status colours (green/yellow/red/gray) appear on nodes with no legend explaining them. A newcomer can't distinguish "idle" from "broken."
- **Fix:** Add four coloured dot + label entries to the topology legend bar: "● Ready  ● Degraded  ● Offline  ● Unknown". Add SVG `<title>` tooltips to each status dot.

### 9. MAJOR — Empty State on First Launch Gives No Guidance
- **Location:** `lib.rs:832` (Agents empty state); topology default (all nodes gray)
- **Gap:** With no agents running, the topology shows 24 gray nodes and Agents shows "No agents found." Users assume the system is broken.
- **Fix:** When all nodes are Unknown, show a banner: "System not yet connected — start QEMU to see live PD status." Add a "Sidebar Stat: 0 / 24 PDs ready" chip.

### 10. MAJOR — Profiler CPU Thresholds Have No Context
- **Location:** `lib.rs:728–730` (60% = hot, 90% = critical); no legend in ProfilerTab
- **Gap:** Colour thresholds are magic numbers invisible to the user. Users can't tell whether 70% CPU is alarming or expected.
- **Fix:** Add a subtitle to the profiler header: "Thresholds: 0–60% normal, 60–90% hot, 90%+ critical." Add help icon linking to Docs > Profiler.

---

## What virt-manager Gets Right — 5 Patterns to Adopt

1. **Sidebar entity list first.** virt-manager's left panel shows every VM with inline status badges. Users immediately know what exists and its state. Adopt: replace or supplement the topology mini-map with a flat list of PD slots + status.

2. **Task-oriented toolbar buttons.** Create VM, Start, Stop, Delete — each with icon + label. Adopt: add "＋ Spawn," "Stop," "View Logs" to each panel's toolbar.

3. **Semantic, labelled status indicators.** Large badges: ✅ Running, ⛔ Stopped, ⚠️ Error. Self-evident without a legend. Adopt: expand 6px dots to badge + short text.

4. **Tooltips on non-obvious terms.** Hovering "Memory" shows "Allocated memory in GiB." Adopt: every PD node name should tooltip with a plain-English description of what that service does.

5. **Grouped, titled sections.** CPU, Memory, Disk, Network groupings with bold titles. Adopt: group Agents panel into "Running / Idle / Failed" sections; group Images into "Cached / Downloading / Missing."

---

## Layout Recommendation: Adopt a Hybrid VM-Row-Centric Design

**Verdict:** The topology-first layout is excellent for architects but poor for operators. Adopt a hybrid:

- **Left sidebar:** Always-visible flat list of PD slots with status badges and CPU bars. Click a slot → opens its terminal and highlights it in topology.
- **Main tabs:**
  - *Console* (default) — xterm tile grid, linked to sidebar selection
  - *Topology* — full PD graph (secondary, not default)
  - *Agents* — list + Spawn/Kill actions
  - *Images* — list + Download/Import actions
  - *Profiler* — CPU table + flame graphs
  - *Docs* — reference

**Key structural changes required:**
1. Make Console the default tab; expose Topology via a "View Graph" button.
2. Add a "＋ Spawn Agent" persistent toolbar button.
3. Expand status indicators from dots to badge+label.
4. Define "PD" inline wherever the term first appears.

---

## Supporting Code Observations

- **Naming inconsistency:** `topology.rs` uses `display` names ("WASM Worker 0") but slot picker in `lib.rs` uses `slot_id` + `display`. The mismatch adds confusion when cross-referencing panels.
- **Missing affordances:** `ImagesPanel` has no import/download button. `AgentsTab` has no spawn dialog.
- **Accessibility:** Unicode sidebar icons (⬡ ⬜ ◈ ⁇) are not semantic — screen readers emit hex codepoint names.
- **Status dot size:** 6px circle is below minimum touch/click target (44×44px) and hard to read on HiDPI displays.
- **Terminology:** "PD," "slot," "swap_slot," "EventBus," "VibeEngine" appear in the UI with no definition at point-of-use.
