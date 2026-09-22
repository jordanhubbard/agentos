# v0.4 integration reconciliation

PR #276 is the protected-main integration candidate. Closing a superseded
topic PR preserves its branch and review history; it does not mean that topic
was independently merged or that v0.4 has been released.

The first 81 superseded PR heads were verified as ancestors of candidate
`eb6f02c34212a23a7817c92aa78ef07a8e817781`. Their numbers are retained in the
integration PR description. The remaining topic histories were reviewed as
follows rather than treated as identical commits:

| PRs | Reconciliation |
| --- | --- |
| #179, #180, #188, #201 | All patches are already present or patch-equivalent, including the restored repeated full-size display test. |
| #186, #187 | Restored host-key generation, VirtIO network/DNS configuration and derived DTB pins. Existing binary payload and x86 profile support was preserved when resolving the documentation conflict. |
| #235 | Restored the guest-written cold-boot witness and terminal systemd failure detection. Adapted the witness argument to current managed-recreation and scenario callers; those callers retain their separate persistence checks. |
| #178 | Current MADT/SSDT generators extend the earlier topology implementation. The host suite still compiles and executes `test_x86_acpi.c`, plus the later firmware-loader and AML checks. |
| #237 | Per-PR supersession concurrency is present. Independent PRs and main pushes retain separate groups. The protected dual-architecture gate replaces the removed duplicate boot jobs. |
| #244, #245 | Functional patches were already included. Historical failures and framebuffer-detach receipts were restored. Later input-probe handling uses `wait_qualification_child` and `expect_input_probe_line`, with stderr context and support for all five input modes, superseding the older diagnostic-only changes. |
| #160, #165, #234, #239, #240, #241, #242, #243, #272, #275 | Functional patches were already included. Restore the historical qualification receipts, including subsequent Intel results and the disclosed build-cache symlink. Preserve current TCB descriptions rather than reinstating older statements that recreation is unavailable. Graphics and disconnect documentation now links the restored observations. |

The restored display test passed exact 1,600-pixel QEMU scanout comparison.
The Debian acquisition recipe passed, and the adapted xtask suite passed
126 unit tests and two integration tests. The version-prepared candidate
passed the workspace command with 372 tests, excluding the three target-only
crates selected by CI. These host and focused results do not replace the
pending persistence runs, required hosted checks, milestone reconciliation,
or exact-revision release checks.
