# v0.4 implementation qualification

These receipts bind results to tested revisions. Final clean-main release
checking, tagging and downloaded-asset verification are separate operations;
these files do not claim that an unpublished release is complete.

| Evidence | Qualified scope |
| --- | --- |
| [Dual recreation](dual-recreation.json) | Core `c2a25050`, local and hosted Ubuntu/FreeBSD authenticated SSH, FreeBSD suspend/resume, surviving peer, fresh Ubuntu handle and rejection of retired handles. Hosted run 35679648789 attempt 2 passed; only the failed SSH job was retried. |
| [Native GUI](native-gui.json) | Core `c2a25050` with external GUI `41b6c1e`: 130 browser tests, 16 Rust tests, native build, actual binary-IPC frame pixels, exact keyboard/pointer delivery, held-input release after process death, reconnect, suspend/resume, pinned SSH and normal shutdown. |
| [Resource profiles](resource-profiles.json) | Measured 2 GiB managed generations, bounded admission rejection, two-vCPU workloads and final-SDK terminal teardown at 2 GiB. Final teardown source `fd7d1dcd` differs from `c2a25050` only in the hosted SSH workflow. Historical evidence retains its original revision. |
| [Control topology](control-topology.mmd) | Initial endpoint grants between CC, manager and primary VMM from the compiled default AArch64 system table. Does not enumerate dynamic mappings, rights changes, hardware grants or live traffic. |

The final-SDK target archive has SHA-256
`fb4290f10c2e59a0baa4d85d477726c3713dec5c497e0d232968bcb6675d566b`.
Packaging verified all nine files in its checksum manifest. Distribution
includes source provenance, the approved CR2 patch and its build recipe.

Reproduce the topology with:

```text
make topology-report TOPOLOGY_PDS='cc_pd vm_manager guest_vmm_primary'
```

The source table SHA-256 is
`3fdb08b0e73103c6feaad54b6e89e1906a03fd11efa4d004a96739677581f140`;
the generated Mermaid SHA-256 is
`01af509610ebe9618dab1f11d5b7711ade04e27d62a3b4c35887445808b9e58f`.
The C reporter links the actual descriptor rather than parsing comments or
reconstructing the topology from prose. CC's direct VMM grant is visible even
though dynamic lifecycle requests use the manager.

Raw logs, selected screenshots and archive hashes are retained under
`/home/jkh/.local/share/agentos-evidence/2026-09-22-dual-recreation`.
Private SSH identities and guest disk images are excluded from receipt
archives. Native timing includes SSH return and host scheduling, so it is not
a one-way latency measurement. Earlier failed attempts remain retained.

Physical GPU/input, arbitrary Intel hardware, x86 desktop graphics, Omarchy,
live memory/device snapshots and statistical reliability are outside these
results. The previously deferred network-desktop workload is distinct from
the qualified direct binary-IPC graphics path.
