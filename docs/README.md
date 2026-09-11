# agentOS documentation index

One line per file. **Current** documents describe the tree as it is and are
kept in step with it. **Historical** documents record an audit, plan, or
design at a past date; they are kept for provenance and are not maintained.
Where a historical document is superseded, the replacement is named. Nothing
here is deleted; do not extend a historical document, update the current one.

If two documents disagree, `TCB.md` wins, then the README, then everything
else.

## Current

| File | What it is |
|------|------------|
| [`TCB.md`](TCB.md) | **Binding.** Trusted computing base: what boots today vs. the target shape, the five I/O invariants, the museum list, the proof gate. |
| [`QUICKSTART.md`](QUICKSTART.md) | Clone to booted QEMU image, guest I/O proofs, logs, troubleshooting. |
| [`DEVELOPER_GUIDE.md`](DEVELOPER_GUIDE.md) | Boot flow, declaring a PD, PD skeleton, contracts, notifications, adding a virtualizer/driver/profile/native agent, testing policy. |
| [`ROADMAP.md`](ROADMAP.md) | Release map 0.2 to 1.0, dependency order, trust-baseline corrective actions, per-release acceptance evidence. |
| [`RELEASES.md`](RELEASES.md) | Evidence-bound release protocol: plan, prepare, check, publish, verify; branch policy; evidence matrix. |
| [`guest-profiles.md`](guest-profiles.md) | Data-driven guest personalities: TOML profile format, host/target boundary, compiler, scenarios, console expect rules. |
| [`demo.md`](demo.md) | The dual-guest Ubuntu + FreeBSD SSH demo: `make setup`, `make demo`, ports, timing, troubleshooting. |
| [`desktop-demo.md`](desktop-demo.md) | Experimental Ubuntu desktop-over-SSH (RFB frame) proof. Not release-qualified. |
| [`linux-guest-baseline.md`](linux-guest-baseline.md) | Roadmap decision: pinned Debian replaces Ubuntu as the Linux integration baseline; artifact and checksum record. |
| [`omarchy-compatibility.md`](omarchy-compatibility.md) | Compatibility ledger for official Omarchy artifacts (roadmap 0.6 input). Facts only, no support claim. |
| [`sel4-loader-format.md`](sel4-loader-format.md) | The `agentos.img` flat image format produced by `cargo xtask gen-image` and parsed by the root task. |
| [`freebsd-vm-guest.md`](freebsd-vm-guest.md) | FreeBSD 15.0 guest: commands are current (`make fetch-guest GUEST_OS=freebsd`, `make run GUEST_OS=freebsd`); the architecture diagram predates the current PD set (names a `freebsd_vm` PD and `controller`/`event_bus`). |
| [`boot-guide.md`](boot-guide.md) | Prerequisites, build steps, target architectures, QEMU interfaces. Partly stale: the "Expected First-Boot Output" markers (`[controller] ... boot complete`) and the "Agent Signing" section describe PDs no longer in the image; use `QUICKSTART.md` for markers. |
| [`presentations/agentos-systems-security/`](presentations/agentos-systems-security/README.md) | Source for the release systems/security deck (`deck.md`) and its claim ledger (`FACTS.md`); rendered by `make presentation-render`. |
| [`defects/`](defects/) | Filed defects. `DEFECT-001` (gpu_shmem, approved exception, now museum) and `DEFECT-002` (js_runtime removal, resolved). |

## Historical (superseded or point-in-time)

| File | Date | What it was | Superseded by |
|------|------|-------------|---------------|
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | April 2026 | Auto-generated PD and contract inventory ("48 PDs in the system file"). The image now boots 13 PDs. | `TCB.md`, README "Booted PD set" |
| [`user-guide/architecture.md`](user-guide/architecture.md) | 2026 | Layered architecture diagram naming `libagent.c` and WASM agents; `libs/libagent` has been deleted. | `TCB.md`, `DEVELOPER_GUIDE.md` |
| [`api-surface-audit.md`](api-surface-audit.md) | 2026-04-15 | Inventory of every service opcode across `userspace/servers/`, `services/`, and the root task. Many of those trees were deleted in the trust-baseline cleanup. | `TCB.md` museum list, `kernel/agentos-root-task/include/contracts/` |
| [`consolidation-plan.md`](consolidation-plan.md) | 2026-04-15 | Plan to merge duplicate services, partially executed. | `CHANGELOG.md` Unreleased "Removed"; `ROADMAP.md` trust baseline |
| [`REFACTORING_PLAN.md`](REFACTORING_PLAN.md) | April 2026 | Repository-wide refactoring plan from the April audit. | `PLAN.md`, `ROADMAP.md` |
| [`VIBEOS_ANALYSIS.md`](VIBEOS_ANALYSIS.md) | April 2026 | Deep dive on the VibeOS dynamic-OS-creation API. VibeOS and the agent-facing services are museum code, not in the booted image. | `TCB.md` museum list |
| [`os-review.md`](os-review.md) | April 2026 | External-style OS design review of v0.1.0-alpha, including the Rust SDK and the "ring" model that `TCB.md` has since rejected. | `TCB.md` |
| [`ui-audit.md`](ui-audit.md) | 2026 | Discoverability audit of the former console dashboard. The dashboard and all in-repo UI were removed under the UI policy. | `CLAUDE.md` UI policy |
| [`device-audit.md`](device-audit.md) | 2026-04-15 | Audit of Linux and FreeBSD VMM device handling against the OS-neutral service rule; references `kernel/freebsd-vmm/`, which no longer exists (one guest-neutral `guest_vmm.c` remains). | `TCB.md` "How I/O flows today" |
| [`libvmm-migration-inventory.md`](libvmm-migration-inventory.md) | 2026 | Inventory of Microkit API call sites in the VMM layer, preparatory to replacing Microkit with direct seL4 syscalls (done: the root task spawns PDs itself). | `DEVELOPER_GUIDE.md` section 1 |
| [`p0-guest-e2e-tasks.md`](p0-guest-e2e-tasks.md) | 2026 | Bring-up record of the first guest E2E gaps. Marked historical in its own header. | `demo.md`, `make demo-test` |
| [`cc-consumer-pattern.md`](cc-consumer-pattern.md) | 2026 | How external tools talk to `cc_pd` over its socket (guest list/status/console opcodes). The opcode descriptions track `contracts/cc_contract.h`; the framing around "web UIs, mobile apps, dashboards" predates the UI policy. | `kernel/agentos-root-task/include/contracts/cc_contract.h`, `tools/agentctl` |
| [`PRIORITY_INHERITANCE.md`](PRIORITY_INHERITANCE.md) | 2026-03-31 | Design note for priority inheritance on passive-PD PPCs under Microkit. The Microkit PPC model is no longer used; priorities are the descriptor's DAG. | `DEVELOPER_GUIDE.md` section 2, descriptor comment in `system_desc_aarch64.c` |

## Top-level documents outside `docs/`

| File | Status |
|------|--------|
| [`../README.md`](../README.md) | Current. Overview, booted PD set, quick start, proof table. |
| [`../CLAUDE.md`](../CLAUDE.md) | Current, binding. Project constitution: TCB, language and UI policy, I/O policy, tests, tracker. |
| [`../AGENTS.md`](../AGENTS.md) | Current, binding. Short-form rules and workflow for contributors and agents. |
| [`../PLAN.md`](../PLAN.md) | Current. Active implementation sequencing with MAC task ids. |
| [`../CHANGELOG.md`](../CHANGELOG.md) | Current. Release notes; Unreleased section lists known limitations. |
| [`../DESIGN.md`](../DESIGN.md) | Vision document from March 2026 (agent identity, capabilities, vibe-coding, agent services). Its status table predates the trust-baseline audit; see the banner at its top. |
| [`../platform/README.md`](../platform/README.md) | Platform I/O tree map. Its table still marks `net_virt` and `blk_virt` as "not in the live image"; both are booted PDs now (`TCB.md`). |
| [`../tests/TARGET_TESTS.md`](../tests/TARGET_TESTS.md) | Current. Host tests vs. source lint vs. target proof. |
| [`../EXTERNAL_CONTRIBUTOR_TEST.md`](../EXTERNAL_CONTRIBUTOR_TEST.md) | Historical. Artifact of a GitHub issue two-way-sync pipeline test; not documentation. |
